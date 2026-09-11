#ifndef EDDS_POOL_H
#define EDDS_POOL_H

#include <edds/edds.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The one worker pool a batch is allowed to own.
 *
 * A converter that spawned a process per image, or that let every image start its own threads,
 * would see all the cores and all the memory as its own and could not be held to either. So the
 * batch runs its images over a single bounded pool: at most `EDDS_POOL_MAX_WORKERS` threads, and
 * one shared byte budget every image must fit inside before it starts decoding.
 */

#define EDDS_POOL_MAX_WORKERS 8u

/** The whole budget one batch process may hold in flight across every worker at once. */
#define EDDS_POOL_MEMORY_BUDGET ((uint64_t)512 * 1024 * 1024)

/**
 * What one source of `source_bytes` is charged against the budget. A compressed image decodes to
 * far more than it weighs on disk, and the mip chain and its staging buffers live beside it, so
 * the charge is deliberately generous rather than a measurement of the decode that has not run.
 */
uint64_t edds_pool_charge_of(uint64_t source_bytes);

typedef struct edds_pool edds_pool;

/**
 * Runs `count` tasks over the pool. `index` is the task's own, so a task touches only what belongs
 * to it. Every task runs exactly once, and the call returns when the last one has finished.
 */
typedef void (*edds_pool_task_fn)(void *context, uint32_t index, edds_pool *pool);

edds_status edds_pool_run(
    uint32_t count,
    uint64_t memory_budget,
    edds_pool_task_fn task,
    void *context,
    edds_error *error
);

/**
 * Holds `bytes` of the shared budget until the matching release. A charge larger than the whole
 * budget is admitted alone rather than refused, so one enormous image still converts; every other
 * worker waits for it. Returns nothing to check: the wait is the whole contract.
 */
void edds_pool_reserve(edds_pool *pool, uint64_t bytes);
void edds_pool_release(edds_pool *pool, uint64_t bytes);

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
