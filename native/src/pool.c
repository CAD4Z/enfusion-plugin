#include <edds/pool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE pool_thread;
typedef CRITICAL_SECTION pool_mutex;
typedef CONDITION_VARIABLE pool_signal;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t pool_thread;
typedef pthread_mutex_t pool_mutex;
typedef pthread_cond_t pool_signal;
#endif

struct edds_pool {
    pool_mutex lock;
    pool_mutex output;
    pool_signal budget_freed;
    uint64_t budget;
    uint64_t held;
    /* Set while an image that needs the whole budget is waiting for the pool to empty. */
    int draining;
    uint32_t count;
    uint32_t next;
    uint32_t workers;
    edds_pool_task_fn task;
    void *context;
};

static void mutex_create(pool_mutex *mutex) {
#ifdef _WIN32
    InitializeCriticalSection(mutex);
#else
    (void)pthread_mutex_init(mutex, NULL);
#endif
}

static void mutex_destroy(pool_mutex *mutex) {
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    (void)pthread_mutex_destroy(mutex);
#endif
}

static void mutex_lock(pool_mutex *mutex) {
#ifdef _WIN32
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

static void mutex_unlock(pool_mutex *mutex) {
#ifdef _WIN32
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

static void signal_create(pool_signal *value) {
#ifdef _WIN32
    InitializeConditionVariable(value);
#else
    (void)pthread_cond_init(value, NULL);
#endif
}

static void signal_destroy(pool_signal *value) {
#ifdef _WIN32
    (void)value;
#else
    (void)pthread_cond_destroy(value);
#endif
}

static void signal_wait(pool_signal *value, pool_mutex *mutex) {
#ifdef _WIN32
    (void)SleepConditionVariableCS(value, mutex, INFINITE);
#else
    (void)pthread_cond_wait(value, mutex);
#endif
}

static void signal_wake_all(pool_signal *value) {
#ifdef _WIN32
    WakeAllConditionVariable(value);
#else
    (void)pthread_cond_broadcast(value);
#endif
}

static uint32_t hardware_workers(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors == 0u ? 1u : (uint32_t)info.dwNumberOfProcessors;
#else
    const long reported = sysconf(_SC_NPROCESSORS_ONLN);
    return reported < 1 ? 1u : (uint32_t)reported;
#endif
}

uint32_t edds_pool_worker_count(uint32_t count) {
    /* A diagnostic and test override only. It is not a protocol field and never a profile one. */
    const char *requested = getenv("EDDS_CONVERT_WORKERS");
    uint32_t workers = hardware_workers();
    if (requested != NULL) {
        const unsigned long asked = strtoul(requested, NULL, 10);
        if (asked >= 1ul && asked <= (unsigned long)EDDS_POOL_MAX_WORKERS) {
            workers = (uint32_t)asked;
        }
    }
    if (workers > EDDS_POOL_MAX_WORKERS) workers = EDDS_POOL_MAX_WORKERS;
    if (count != 0u && workers > count) workers = count;
    return workers == 0u ? 1u : workers;
}

uint64_t edds_pool_charge_of(uint64_t source_bytes) {
    /* Four bytes a pixel, a mip chain a third as large again, and the staging the encoder wants. */
    const uint64_t base = (uint64_t)4 * 1024 * 1024;
    const uint64_t expansion = 8u;
    if (source_bytes > (UINT64_MAX - base) / expansion) return UINT64_MAX;
    return source_bytes * expansion + base;
}

uint32_t edds_pool_workers(edds_pool *pool) {
    uint32_t workers;
    if (pool == NULL) return 0u;
    mutex_lock(&pool->lock);
    workers = pool->workers;
    mutex_unlock(&pool->lock);
    return workers;
}

void edds_pool_reserve(edds_pool *pool, uint64_t bytes) {
    uint64_t charged;
    if (pool == NULL || bytes == 0u) return;
    mutex_lock(&pool->lock);
    charged = bytes > pool->budget ? pool->budget : bytes;
    if (charged == pool->budget) {
        /*
         * One image needing the whole budget still runs, alone. Nothing new may start while it
         * waits for the pool to empty, or the workers cycling short images beside it would keep
         * taking the room back and it would never get its turn.
         */
        while (pool->draining) signal_wait(&pool->budget_freed, &pool->lock);
        pool->draining = 1;
        while (pool->held != 0u) signal_wait(&pool->budget_freed, &pool->lock);
        pool->held = charged;
        pool->draining = 0;
        signal_wake_all(&pool->budget_freed);
        mutex_unlock(&pool->lock);
        return;
    }
    while (pool->draining || charged > pool->budget - pool->held) {
        signal_wait(&pool->budget_freed, &pool->lock);
    }
    pool->held += charged;
    mutex_unlock(&pool->lock);
}

void edds_pool_release(edds_pool *pool, uint64_t bytes) {
    uint64_t charged;
    if (pool == NULL || bytes == 0u) return;
    mutex_lock(&pool->lock);
    charged = bytes > pool->budget ? pool->budget : bytes;
    pool->held = charged > pool->held ? 0u : pool->held - charged;
    signal_wake_all(&pool->budget_freed);
    mutex_unlock(&pool->lock);
}

void edds_pool_lock_output(edds_pool *pool) {
    if (pool != NULL) mutex_lock(&pool->output);
}

void edds_pool_unlock_output(edds_pool *pool) {
    if (pool != NULL) mutex_unlock(&pool->output);
}

static void run_tasks(edds_pool *pool) {
    for (;;) {
        uint32_t index;
        mutex_lock(&pool->lock);
        if (pool->next >= pool->count) {
            mutex_unlock(&pool->lock);
            return;
        }
        index = pool->next++;
        mutex_unlock(&pool->lock);
        pool->task(pool->context, index, pool);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID context) {
    run_tasks((edds_pool *)context);
    return 0;
}
#else
static void *worker_entry(void *context) {
    run_tasks((edds_pool *)context);
    return NULL;
}
#endif

static int thread_start(pool_thread *thread, edds_pool *pool) {
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, worker_entry, pool, 0, NULL);
    return *thread != NULL;
#else
    return pthread_create(thread, NULL, worker_entry, pool) == 0;
#endif
}

static void thread_join(pool_thread thread) {
#ifdef _WIN32
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
}

edds_status edds_pool_run(
    uint32_t count,
    uint64_t memory_budget,
    edds_pool_task_fn task,
    void *context,
    edds_error *error
) {
    edds_pool pool;
    pool_thread threads[EDDS_POOL_MAX_WORKERS];
    uint32_t started = 0;
    if (task == NULL || error == NULL) return EDDS_INTERNAL_FAILURE;
    if (count == 0u) return EDDS_OK;

    memset(&pool, 0, sizeof pool);
    pool.budget = memory_budget == 0u ? EDDS_POOL_MEMORY_BUDGET : memory_budget;
    pool.count = count;
    pool.task = task;
    pool.context = context;
    pool.workers = edds_pool_worker_count(count);
    mutex_create(&pool.lock);
    mutex_create(&pool.output);
    signal_create(&pool.budget_freed);

    /* The caller's own thread is one of the workers, so a single-worker pool starts none. */
    while (started + 1u < pool.workers) {
        if (!thread_start(&threads[started], &pool)) break;
        ++started;
    }
    if (started + 1u != pool.workers) {
        /* A thread the system refused is one fewer worker, and every reader is told so. */
        mutex_lock(&pool.lock);
        pool.workers = started + 1u;
        mutex_unlock(&pool.lock);
    }
    run_tasks(&pool);
    for (uint32_t at = 0; at < started; ++at) thread_join(threads[at]);

    signal_destroy(&pool.budget_freed);
    mutex_destroy(&pool.output);
    mutex_destroy(&pool.lock);
    if (pool.next < count) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "batch-pool-incomplete");
        (void)snprintf(error->message, sizeof error->message,
            "The batch worker pool did not run every job.");
        return EDDS_INTERNAL_FAILURE;
    }
    return EDDS_OK;
}
