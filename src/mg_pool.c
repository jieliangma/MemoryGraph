/*
 * mg_pool.c — Pre-allocated memory pool (bump allocator)
 *
 * Uses mmap to allocate a contiguous block at init time.
 * All allocations during collection come from this pool via
 * a simple bump pointer, avoiding any malloc/free calls.
 */

#include "mg_pool.h"
#include <sys/mman.h>
#include <string.h>

#define MG_POOL_ALIGN 8

struct mg_pool {
    uint8_t *base;      /* start of mmap'd region           */
    size_t   capacity;  /* total bytes                      */
    size_t   offset;    /* next allocation offset           */
    bool     truncated; /* set if an allocation ever failed */
};

/* The pool header lives at the start of the mmap'd region. */
#define HEADER_SIZE \
    (((sizeof(struct mg_pool)) + MG_POOL_ALIGN - 1) & ~((size_t)(MG_POOL_ALIGN - 1)))

mg_pool_t *mg_pool_create(size_t size) {
    if (size < HEADER_SIZE + MG_POOL_ALIGN) {
        return NULL;
    }

    void *mem = mmap(NULL, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON,
                     -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }

    mg_pool_t *pool = (mg_pool_t *)mem;
    pool->base      = (uint8_t *)mem;
    pool->capacity  = size;
    pool->offset    = HEADER_SIZE;
    pool->truncated = false;
    return pool;
}

void *mg_pool_alloc(mg_pool_t *pool, size_t size) {
    if (!pool || size == 0) {
        return NULL;
    }

    /* align up */
    size_t aligned = (size + MG_POOL_ALIGN - 1) & ~((size_t)(MG_POOL_ALIGN - 1));
    size_t new_offset = pool->offset + aligned;

    if (new_offset > pool->capacity) {
        pool->truncated = true;
        return NULL;
    }

    void *ptr = pool->base + pool->offset;
    pool->offset = new_offset;
    return ptr;
}

void mg_pool_reset(mg_pool_t *pool) {
    if (!pool) return;
    pool->offset    = HEADER_SIZE;
    pool->truncated = false;
}

size_t mg_pool_used(const mg_pool_t *pool) {
    return pool ? pool->offset - HEADER_SIZE : 0;
}

size_t mg_pool_capacity(const mg_pool_t *pool) {
    return pool ? pool->capacity - HEADER_SIZE : 0;
}

size_t mg_pool_remaining(const mg_pool_t *pool) {
    return pool ? pool->capacity - pool->offset : 0;
}

bool mg_pool_truncated(const mg_pool_t *pool) {
    return pool ? pool->truncated : false;
}

void mg_pool_destroy(mg_pool_t *pool) {
    if (!pool) return;
    munmap(pool->base, pool->capacity);
    /* pool pointer is now invalid */
}
