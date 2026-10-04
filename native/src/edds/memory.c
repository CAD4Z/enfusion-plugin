#include "memory.h"

#include <stdlib.h>
#include <string.h>

typedef struct memory_scope     memory_scope;
typedef union allocation_header allocation_header;

union allocation_header {
    long double alignment;

    struct {
        size_t             bytes;
        memory_scope      *owner;
        allocation_header *previous;
        allocation_header *next;
    } value;
};

struct memory_scope {
    uint64_t           limit;
    uint64_t           used;
    uint64_t           peak;
    uint64_t           required;
    allocation_header *head;
};

static _Thread_local memory_scope *active_scope;

void *edds_alloc(size_t size) {
    allocation_header *header;
    memory_scope      *scope = active_scope;
    size_t             bytes;
    if (size > SIZE_MAX - sizeof *header) {
        return NULL;
    }
    bytes = size + sizeof *header;
    if (scope != NULL) {
        if (scope->required != 0u) {
            return NULL;
        }
        if ((uint64_t)bytes > scope->limit - scope->used) {
            scope->required = (uint64_t)bytes > UINT64_MAX - scope->used
                ? UINT64_MAX
                : scope->used + (uint64_t)bytes;
            return NULL;
        }
    }
    header = malloc(bytes);
    if (header == NULL) {
        return NULL;
    }
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
    return header + 1;
}

void edds_free(void *allocation) {
    allocation_header *header;
    memory_scope      *scope;
    if (allocation == NULL) {
        return;
    }
    header = (allocation_header *)allocation - 1;
    scope  = header->value.owner;
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
    header   = (allocation_header *)allocation - 1;
    previous = header->value.bytes - sizeof *header;
    /* Both blocks coexist while copying; reserve the real peak, not just their difference. */
    grown    = edds_alloc(size);
    if (grown == NULL) {
        return NULL;
    }
    memcpy(grown, allocation, size < previous ? size : previous);
    edds_free(allocation);
    return grown;
}

edds_memory_result edds_memory_run(
    uint64_t limit, edds_memory_operation_fn operation, void *context, edds_error *error) {
    memory_scope       scope  = { 0 };
    edds_memory_result result = { EDDS_INTERNAL_FAILURE, 0, 0 };
    if (active_scope != NULL || operation == NULL || error == NULL) {
        if (error != NULL) {
            memset(error, 0, sizeof *error);
            (void)snprintf(error->code, sizeof error->code, "memory-scope-misuse");
            (void)snprintf(error->message, sizeof error->message,
                "A codec memory scope was opened inside another one.");
        }
        return result;
    }
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
        (void)snprintf(error->message, sizeof error->message,
            "The conversion did not release its working memory.");
    }
    active_scope = NULL;
    return result;
}
