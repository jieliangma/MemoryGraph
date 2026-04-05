/*
 * mg_pool.h — Pre-allocated memory pool (bump allocator)
 *
 * All memory used during snapshot collection comes from this pool.
 * No malloc/free calls during collection.
 */

#ifndef MG_POOL_H
#define MG_POOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque pool handle */
typedef struct mg_pool mg_pool_t;

/*
 * Create a pool by mmap-ing `size` bytes.
 * Returns NULL on failure.
 */
mg_pool_t *mg_pool_create(size_t size);

/*
 * Allocate `size` bytes from the pool (8-byte aligned).
 * Returns NULL if the pool is exhausted.
 */
void *mg_pool_alloc(mg_pool_t *pool, size_t size);

/*
 * Reset the pool — all previous allocations become invalid.
 * The underlying memory is retained for reuse.
 */
void mg_pool_reset(mg_pool_t *pool);

/*
 * Returns bytes currently allocated (used).
 */
size_t mg_pool_used(const mg_pool_t *pool);

/*
 * Returns total capacity of the pool.
 */
size_t mg_pool_capacity(const mg_pool_t *pool);

/*
 * Returns remaining free bytes.
 */
size_t mg_pool_remaining(const mg_pool_t *pool);

/*
 * Returns true if the pool has been exhausted at any point
 * (allocation returned NULL).
 */
bool mg_pool_truncated(const mg_pool_t *pool);

/*
 * Destroy the pool and munmap the memory.
 */
void mg_pool_destroy(mg_pool_t *pool);

#endif /* MG_POOL_H */
