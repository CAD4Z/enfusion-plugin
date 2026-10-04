#ifndef EDDS_INTERNAL_MEMORY_H
#define EDDS_INTERNAL_MEMORY_H

#include <stddef.h>
#include <edds/memory.h>

void *edds_alloc(size_t size);
void *edds_calloc(size_t count, size_t size);
void *edds_realloc(void *allocation, size_t size);

#endif
