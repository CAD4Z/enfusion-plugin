#include <edds/pool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The thread, the lock and the wake-up signal of each system, under one set of names. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE             pool_thread;
typedef CRITICAL_SECTION   pool_mutex;
typedef CONDITION_VARIABLE pool_signal;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t       pool_thread;
typedef pthread_mutex_t pool_mutex;
typedef pthread_cond_t  pool_signal;
#endif

/** The pool every worker of one run shares. */
struct edds_pool {
    /**
     * The lock over the numbers below that change while tasks run, the lock tasks hold while they
     * write output, and the signal that wakes the workers waiting for budget.
     */
    pool_mutex  lock;
    pool_mutex  output;
    pool_signal budget_freed;

    /** The byte budget, and how much of it the running tasks hold. */
    uint64_t budget;
    uint64_t held;

    /** Set while an image that needs the whole budget is waiting for the pool to empty. */
    int draining;

    /** How many tasks there are, the index of the next one to hand out, and the workers. */
    uint32_t count;
    uint32_t next;
    uint32_t workers;

    /** What every task runs, and the context it is given. */
    edds_pool_task_fn task;
    void             *context;
};

/** Sets up a lock. */
static void mutex_create(pool_mutex *mutex) {
#ifdef _WIN32
    InitializeCriticalSection(mutex);
#else
    (void)pthread_mutex_init(mutex, NULL);
#endif
}

/** Tears down a lock nothing holds any more. */
static void mutex_destroy(pool_mutex *mutex) {
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    (void)pthread_mutex_destroy(mutex);
#endif
}

/** Takes a lock, waiting while another thread holds it. */
static void mutex_lock(pool_mutex *mutex) {
#ifdef _WIN32
    EnterCriticalSection(mutex);
#else
    (void)pthread_mutex_lock(mutex);
#endif
}

/** Lets a lock go. */
static void mutex_unlock(pool_mutex *mutex) {
#ifdef _WIN32
    LeaveCriticalSection(mutex);
#else
    (void)pthread_mutex_unlock(mutex);
#endif
}

/** Sets up a signal that threads can wait on. */
static void signal_create(pool_signal *value) {
#ifdef _WIN32
    InitializeConditionVariable(value);
#else
    (void)pthread_cond_init(value, NULL);
#endif
}

/** Tears down a signal; on Windows there is nothing to tear down. */
static void signal_destroy(pool_signal *value) {
#ifdef _WIN32
    (void)value;
#else
    (void)pthread_cond_destroy(value);
#endif
}

/** Lets `mutex` go and sleeps until the signal is raised, then takes `mutex` back. */
static void signal_wait(pool_signal *value, pool_mutex *mutex) {
#ifdef _WIN32
    (void)SleepConditionVariableCS(value, mutex, INFINITE);
#else
    (void)pthread_cond_wait(value, mutex);
#endif
}

/** Raises the signal: every thread waiting on it wakes. */
static void signal_wake_all(pool_signal *value) {
#ifdef _WIN32
    WakeAllConditionVariable(value);
#else
    (void)pthread_cond_broadcast(value);
#endif
}

/** The number of processors the system reports, and 1 when it reports none. */
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

/**
 * One worker per processor, or as many as EDDS_CONVERT_WORKERS asks for; never more than
 * EDDS_POOL_MAX_WORKERS, nor more than `count` tasks need. At least 1.
 */
uint32_t edds_pool_worker_count(uint32_t count) {
    /* A diagnostic and test override only. It is not a protocol field and never a profile one. */
    const char *requested = getenv("EDDS_CONVERT_WORKERS");
    uint32_t    workers   = hardware_workers();

    /* The override counts only when it is a number from 1 to EDDS_POOL_MAX_WORKERS. */
    if (requested != NULL) {
        const unsigned long asked = strtoul(requested, NULL, 10);

        if (asked >= 1ul && asked <= (unsigned long)EDDS_POOL_MAX_WORKERS) {
            workers = (uint32_t)asked;
        }
    }

    if (workers > EDDS_POOL_MAX_WORKERS) {
        workers = EDDS_POOL_MAX_WORKERS;
    }

    if (count != 0u && workers > count) {
        workers = count;
    }

    return workers == 0u ? 1u : workers;
}

/** Eight times the source's size plus 4 MiB; UINT64_MAX when that would not fit 64 bits. */
uint64_t edds_pool_charge_of(uint64_t source_bytes) {
    /* A cheap initial hint. Only the enforced allocation quota can bound compressed sources. */
    const uint64_t base      = (uint64_t)4 * 1024 * 1024;
    const uint64_t expansion = 8u;

    if (source_bytes > (UINT64_MAX - base) / expansion) {
        return UINT64_MAX;
    }

    return source_bytes * expansion + base;
}

/** The pool's worker count, read under its lock; 0 without a pool. */
uint32_t edds_pool_workers(edds_pool *pool) {
    uint32_t workers;

    if (pool == NULL) {
        return 0u;
    }

    mutex_lock(&pool->lock);
    workers = pool->workers;
    mutex_unlock(&pool->lock);

    return workers;
}

/**
 * Waits until `bytes` fit beside what the running tasks hold, then adds them to what is held. A
 * charge of the whole budget or more counts as the whole budget, and waits for the pool to empty.
 */
void edds_pool_reserve(edds_pool *pool, uint64_t bytes) {
    uint64_t charged;

    if (pool == NULL || bytes == 0u) {
        return;
    }

    mutex_lock(&pool->lock);
    charged = bytes > pool->budget ? pool->budget : bytes;

    if (charged == pool->budget) {
        /*
         * One image needing the whole budget still runs, alone. Nothing new may start while it
         * waits for the pool to empty, or the workers cycling short images beside it would keep
         * taking the room back and it would never get its turn.
         */
        while (pool->draining) {
            signal_wait(&pool->budget_freed, &pool->lock);
        }

        pool->draining = 1;

        while (pool->held != 0u) {
            signal_wait(&pool->budget_freed, &pool->lock);
        }

        /* The pool is empty: this image takes all of it, and the waiting workers look again. */
        pool->held     = charged;
        pool->draining = 0;
        signal_wake_all(&pool->budget_freed);
        mutex_unlock(&pool->lock);
        return;
    }

    /* Any other charge waits until no image is draining the pool and there is room for it. */
    while (pool->draining || charged > pool->budget - pool->held) {
        signal_wait(&pool->budget_freed, &pool->lock);
    }

    pool->held += charged;
    mutex_unlock(&pool->lock);
}

/**
 * Gives back what edds_pool_reserve took for `bytes`, never taking the held total below zero,
 * and wakes every worker waiting for room.
 */
void edds_pool_release(edds_pool *pool, uint64_t bytes) {
    uint64_t charged;

    if (pool == NULL || bytes == 0u) {
        return;
    }

    mutex_lock(&pool->lock);
    charged    = bytes > pool->budget ? pool->budget : bytes;
    pool->held = charged > pool->held ? 0u : pool->held - charged;
    signal_wake_all(&pool->budget_freed);
    mutex_unlock(&pool->lock);
}

/**
 * Runs `operation` in attempts, each holding a charge of the budget, `initial_charge` at first, as
 * its allocation quota. When the quota is what cut an attempt short, the charge doubles (stopping
 * at the whole budget when that is enough, and never below what the attempt needed) and the
 * operation runs again.
 */
edds_status edds_pool_execute(edds_pool *pool, uint64_t initial_charge,
    edds_memory_operation_fn operation, void *context,
    edds_cancelled_fn cancelled, void *cancel_context, edds_error *error) {
    uint64_t charge = initial_charge == 0u ? 1u : initial_charge;

    if (pool == NULL || operation == NULL || error == NULL) {
        return EDDS_INTERNAL_FAILURE;
    }

    for (;;) {
        edds_memory_result result;
        uint64_t           next;

        /* Room in the budget first; a batch cancelled meanwhile starts no attempt. */
        edds_pool_reserve(pool, charge);

        if (cancelled != NULL && cancelled(cancel_context)) {
            edds_pool_release(pool, charge);
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "cancelled");
            (void)snprintf(error->message, sizeof error->message, "The batch was cancelled before this conversion attempt started.");
            return EDDS_CANCELLED;
        }

        /* One attempt. A charge over the whole budget runs alone, with no quota at all. */
        result = edds_memory_run(charge > pool->budget ? UINT64_MAX : charge, operation, context, error);
        edds_pool_release(pool, charge);

        /* Done, unless the quota refused an allocation; `required` is then what it needed. */
        if (result.status == EDDS_OK || result.required == 0u) {
            return result.status;
        }

        /*
         * Never wait for more memory while holding decoded buffers: that could deadlock workers.
         * The operation has rolled back, and memory_run has verified that its buffers are gone.
         */
        next = charge > UINT64_MAX / 2u ? UINT64_MAX : charge * 2u;

        if (next > pool->budget && charge < pool->budget && result.required <= pool->budget) {
            next = pool->budget;
        }

        charge = next < result.required ? result.required : next;
    }
}

/** Takes the output lock; without a pool, does nothing. */
void edds_pool_lock_output(edds_pool *pool) {
    if (pool != NULL) {
        mutex_lock(&pool->output);
    }
}

/** Lets the output lock go; without a pool, does nothing. */
void edds_pool_unlock_output(edds_pool *pool) {
    if (pool != NULL) {
        mutex_unlock(&pool->output);
    }
}

/** One worker's loop: takes the next task that nobody has taken and runs it, until none is left. */
static void run_tasks(edds_pool *pool) {
    for (;;) {
        uint32_t index;

        /* The next index is taken under the lock, so every task goes to exactly one worker. */
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

/** Where a started thread begins: it works through the tasks like the calling thread does. */
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

/** Starts one worker thread on the pool: 1 when the system started it, 0 when it refused. */
static int thread_start(pool_thread *thread, edds_pool *pool) {
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, worker_entry, pool, 0, NULL);

    return *thread != NULL;
#else
    return pthread_create(thread, NULL, worker_entry, pool) == 0;
#endif
}

/** Waits for a worker thread to finish, and lets the system free it. */
static void thread_join(pool_thread thread) {
#ifdef _WIN32
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
}

/**
 * Sets the pool up, starts its threads, works through the tasks on the calling thread as well,
 * then waits for every thread and tears the pool down. EDDS_OK once every task has run.
 */
edds_status edds_pool_run(
    uint32_t          count,
    uint64_t          memory_budget,
    edds_pool_task_fn task,
    void             *context,
    edds_error       *error) {
    edds_pool   pool;
    pool_thread threads[EDDS_POOL_MAX_WORKERS];
    uint32_t    started = 0;

    if (task == NULL || error == NULL) {
        return EDDS_INTERNAL_FAILURE;
    }

    if (count == 0u) {
        return EDDS_OK;
    }

    /* The pool: its budget (0 asks for the default), its tasks, its workers and its locks. */
    memset(&pool, 0, sizeof pool);
    pool.budget  = memory_budget == 0u ? EDDS_POOL_MEMORY_BUDGET : memory_budget;
    pool.count   = count;
    pool.task    = task;
    pool.context = context;
    pool.workers = edds_pool_worker_count(count);
    mutex_create(&pool.lock);
    mutex_create(&pool.output);
    signal_create(&pool.budget_freed);

    /* The caller's own thread is one of the workers, so a single-worker pool starts none. */
    while (started + 1u < pool.workers) {
        if (!thread_start(&threads[started], &pool)) {
            break;
        }

        ++started;
    }

    if (started + 1u != pool.workers) {
        /* A thread the system refused is one fewer worker, and every reader is told so. */
        mutex_lock(&pool.lock);
        pool.workers = started + 1u;
        mutex_unlock(&pool.lock);
    }

    /* The calling thread works too, then waits for the threads it started. */
    run_tasks(&pool);

    for (uint32_t at = 0; at < started; ++at) {
        thread_join(threads[at]);
    }

    signal_destroy(&pool.budget_freed);
    mutex_destroy(&pool.output);
    mutex_destroy(&pool.lock);

    /* Every task index must have been handed out. */
    if (pool.next < count) {
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "batch-pool-incomplete");
        (void)snprintf(error->message, sizeof error->message, "The batch worker pool did not run every job.");
        return EDDS_INTERNAL_FAILURE;
    }

    return EDDS_OK;
}
