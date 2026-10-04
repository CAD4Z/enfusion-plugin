#ifndef EDDS_MEMORY_H
#define EDDS_MEMORY_H

#include <edds/edds.h>

/** The work `edds_memory_run` runs: it gets back the caller's `context` and returns its status. */
typedef edds_status (*edds_memory_operation_fn)(void *context, edds_error *error);

/**
 * What one run reports: the operation's status, or a failure of the run itself, and the most
 * bytes its allocations held at one time, bookkeeping included.
 */
typedef struct edds_memory_result {
    edds_status status;
    uint64_t    peak;
    /**
     * Nonzero only when the quota, rather than the system allocator, refused an allocation. It is
     * then the quota that allocation would have needed: the bytes already held plus its own.
     */
    uint64_t    required;
} edds_memory_result;

/**
 * Runs a synchronous operation with a thread-local allocation quota. All codec allocations,
 * including their bookkeeping, count before they are made. The operation must release them
 * before returning and roll back any output on failure so a caller can retry with more room.
 * UINT64_MAX permits an oversized operation admitted alone by the worker pool.
 */
edds_memory_result edds_memory_run(uint64_t limit, edds_memory_operation_fn operation,
    void *context, edds_error *error);

#endif
