#ifndef EDDS_INTERNAL_MEMORY_H
#define EDDS_INTERNAL_MEMORY_H

#include <stddef.h>
#include <edds/memory.h>

/**
 * Allocates `size` bytes, counted against the quota of the `edds_memory_run` in progress on this
 * thread, if there is one. Returns NULL when the quota or the system refuses; `edds_free` releases
 * what it returns.
 */
void *edds_alloc(size_t size);

/** As `edds_alloc`, for `count` items of `size` bytes each, every byte set to zero. */
void *edds_calloc(size_t count, size_t size);

/**
 * Moves an allocation into a new one of `size` bytes, keeping the contents that fit. A NULL
 * `allocation` is a plain allocation, and a `size` of 0 frees it and returns NULL. When the new
 * allocation is refused, returns NULL and leaves the old one as it was.
 */
void *edds_realloc(void *allocation, size_t size);

#endif
