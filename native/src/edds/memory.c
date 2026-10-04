/*
 * The allocator every codec allocation goes through. Each allocation carries a header in front of
 * it; inside an `edds_memory_run` the header links it into the run's list and its bytes count
 * against the run's quota, so the run can see what is still held when the operation returns.
 */
#include "memory.h"

#include <stdlib.h>
#include <string.h>

typedef struct memory_scope     memory_scope;
typedef union allocation_header allocation_header;

/**
 * Sits right in front of every allocation: its size with this header included, the run that
 * counts it (NULL outside a run), and its neighbours in that run's list. The `alignment` member
 * only pads: it makes the header, and so the bytes after it, aligned like a `long double`.
 */
union allocation_header {
    long double alignment;

    struct {
        size_t             bytes;
        memory_scope      *owner;
        allocation_header *previous;
        allocation_header *next;
    } value;
};

/**
 * One `edds_memory_run` in progress: its quota, the bytes held now and the most held at one time,
 * the quota a refused allocation would have needed, and the newest allocation, at the head of the
 * list of all it holds.
 */
struct memory_scope {
    uint64_t           limit;
    uint64_t           used;
    uint64_t           peak;
    uint64_t           required;
    allocation_header *head;
};

/** The run in progress on this thread, or NULL outside one. */
static _Thread_local memory_scope *active_scope;

/**
 * Allocates `size` bytes behind a header. Inside a run they count against its quota, and once the
 * quota has refused one allocation it refuses every later one too. Returns NULL when refused.
 */
void *edds_alloc(size_t size) {
    allocation_header *header;
    memory_scope      *scope = active_scope;
    size_t             bytes;

    /* The size with the header added must still fit a size_t. */
    if (size > SIZE_MAX - sizeof *header) {
        return NULL;
    }

    bytes = size + sizeof *header;

    /* Inside a run, a refusal records the quota the run would have needed. */
    if (scope != NULL) {
        if (scope->required != 0u) {
            return NULL;
        }

        if ((uint64_t)bytes > scope->limit - scope->used) {
            scope->required = (uint64_t)bytes > UINT64_MAX - scope->used ? UINT64_MAX : scope->used + (uint64_t)bytes;
            return NULL;
        }
    }

    header = malloc(bytes);

    if (header == NULL) {
        return NULL;
    }

    /* The new allocation goes to the head of the run's list, and its bytes are counted. */
    header->value.bytes    = bytes;
    header->value.owner    = scope;
    header->value.previous = NULL;
    header->value.next     = scope == NULL ? NULL : scope->head;

    if (scope != NULL) {
        if (scope->head != NULL) {
            scope->head->value.previous = header;
        }

        scope->head  = header;
        scope->used += bytes;

        if (scope->used > scope->peak) {
            scope->peak = scope->used;
        }
    }

    /* The caller gets the bytes right after the header. */
    return header + 1;
}

/** Releases an allocation, taking it off its run's list and count; NULL is ignored. */
void edds_free(void *allocation) {
    allocation_header *header;
    memory_scope      *scope;

    if (allocation == NULL) {
        return;
    }

    /* The header sits right in front of the bytes the caller was given. */
    header = (allocation_header *)allocation - 1;
    scope  = header->value.owner;

    /* An allocation made inside a run leaves the run's list and count. */
    if (scope != NULL) {
        if (header->value.previous != NULL) {
            header->value.previous->value.next = header->value.next;
        } else {
            scope->head = header->value.next;
        }

        if (header->value.next != NULL) {
            header->value.next->value.previous = header->value.previous;
        }

        scope->used -= header->value.bytes;
    }

    free(header);
}

/**
 * Allocates `count` items of `size` bytes, all zero. Returns NULL when their total overflows or
 * the allocation is refused.
 */
void *edds_calloc(size_t count, size_t size) {
    void *allocation;

    if (size != 0u && count > SIZE_MAX / size) {
        return NULL;
    }

    allocation = edds_alloc(count * size);

    if (allocation != NULL) {
        memset(allocation, 0, count * size);
    }

    return allocation;
}

/**
 * Moves an allocation into a new one of `size` bytes and frees the old, keeping the contents that
 * fit. NULL allocates anew, and a `size` of 0 frees and returns NULL. When the new allocation is
 * refused, returns NULL and the old one stays as it was.
 */
void *edds_realloc(void *allocation, size_t size) {
    allocation_header *header;
    void              *grown;
    size_t             previous;

    if (allocation == NULL) {
        return edds_alloc(size);
    }

    if (size == 0u) {
        edds_free(allocation);
        return NULL;
    }

    /* The size the caller asked for before: what the header counts, less the header itself. */
    header   = (allocation_header *)allocation - 1;
    previous = header->value.bytes - sizeof *header;

    /* Both blocks coexist while copying; reserve the real peak, not just their difference. */
    grown = edds_alloc(size);

    if (grown == NULL) {
        return NULL;
    }

    memcpy(grown, allocation, size < previous ? size : previous);
    edds_free(allocation);

    return grown;
}

/**
 * Runs `operation` with every allocation on this thread counted against `limit`, and returns its
 * status with the peak and any refused quota. A run inside another run is refused, and memory the
 * operation leaves allocated is freed and turns its status into a failure.
 */
edds_memory_result edds_memory_run(uint64_t limit, edds_memory_operation_fn operation, void *context, edds_error *error) {
    memory_scope       scope  = { 0 };
    edds_memory_result result = { EDDS_INTERNAL_FAILURE, 0, 0 };

    /* Refused inside another run on this thread, or without an operation or an error to fill. */
    if (active_scope != NULL || operation == NULL || error == NULL) {
        if (error != NULL) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "memory-scope-misuse");
            (void)snprintf(error->message, sizeof error->message, "A codec memory scope was opened inside another one.");
        }

        return result;
    }

    /* The operation runs with this scope active, so everything it allocates is counted. */
    scope.limit     = limit;
    active_scope    = &scope;
    result.status   = operation(context, error);
    result.peak     = scope.peak;
    result.required = scope.required;

    if (scope.head != NULL) {
        /* A broken operation cannot leak memory into the next job or keep a dangling owner. */
        while (scope.head != NULL) {
            edds_free(scope.head + 1);
        }

        result.status   = EDDS_INTERNAL_FAILURE;
        result.required = 0;
        memset(error, 0, sizeof *error);
        (void)snprintf(error->code, sizeof error->code, "operation-memory-leak");
        (void)snprintf(error->message, sizeof error->message, "The conversion did not release its working memory.");
    }

    active_scope = NULL;

    return result;
}
