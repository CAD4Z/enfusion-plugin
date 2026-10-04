/*
 * The one worker pool a batch is allowed to own.
 *
 * A converter that spawned a process per image, or that let every image start its own threads,
 * would see all the cores and all the memory as its own and could not be held to either. So the
 * batch runs its images over a single bounded pool: at most `EDDS_POOL_MAX_WORKERS` threads, and
 * one shared byte budget every image must fit inside before it starts decoding.
 */
#ifndef EDDS_POOL_H
#define EDDS_POOL_H

#include <edds/edds.h>
#include <edds/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The most workers one pool runs; the thread that calls edds_pool_run is one of them. */
#define EDDS_POOL_MAX_WORKERS 8u

/** The whole budget one batch process may hold in flight across every worker at once. */
#define EDDS_POOL_MEMORY_BUDGET ((uint64_t)512 * 1024 * 1024)

/**
 * Initial quota for a source of `source_bytes`. This is only an admission hint, not a bound on
 * decoded memory: pool_execute enforces the quota on actual allocations and grows it on retry.
 */
uint64_t edds_pool_charge_of(uint64_t source_bytes);

/** A running pool. Tasks get it from edds_pool_run and hand it back to the calls below. */
typedef struct edds_pool edds_pool;

/** One task: `context` is what the caller passed to edds_pool_run, `index` which task this is. */
typedef void (*edds_pool_task_fn)(void *context, uint32_t index, edds_pool *pool);

/**
 * Runs `count` tasks over the pool. `index` is the task's own, so a task touches only what belongs
 * to it. Every task runs exactly once, and the call returns when the last one has finished.
 * A `memory_budget` of 0 means EDDS_POOL_MEMORY_BUDGET. Returns EDDS_OK once every task has run.
 */
edds_status edds_pool_run(
    uint32_t          count,
    uint64_t          memory_budget,
    edds_pool_task_fn task,
    void             *context,
    edds_error       *error);

/**
 * Holds `bytes` of the shared budget until the matching release. A charge larger than the whole
 * budget is admitted alone rather than refused, so one enormous image still converts; every other
 * worker waits for it. Returns nothing to check: the wait is the whole contract.
 * edds_pool_release gives the bytes back.
 */
void edds_pool_reserve(edds_pool *pool, uint64_t bytes);
void edds_pool_release(edds_pool *pool, uint64_t bytes);

/**
 * Executes an atomic operation under an enforced allocation quota. If the initial charge is too
 * small, the failed attempt releases its allocations and temps before retrying with more room.
 * An operation exceeding the entire budget runs alone. Cancellation is checked after waiting for
 * admission and before every attempt; ordinary input/IO/allocator failures are never retried.
 * Returns the status of the last attempt, or EDDS_CANCELLED when cancellation stopped one from
 * starting.
 */
edds_status edds_pool_execute(edds_pool *pool, uint64_t initial_charge,
    edds_memory_operation_fn operation, void *context,
    edds_cancelled_fn cancelled, void *cancel_context, edds_error *error);

/**
 * Serializes whatever a task writes to a shared stream. Events are several writes each, so the
 * lock has to span a whole event, not one write of it.
 */
void edds_pool_lock_output(edds_pool *pool);
void edds_pool_unlock_output(edds_pool *pool);

/** How many workers the pool actually started, which a task reads back to prove the bound. */
uint32_t edds_pool_workers(edds_pool *pool);

/** The workers this build would start for `count` tasks, without starting any of them. */
uint32_t edds_pool_worker_count(uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
