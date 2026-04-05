/*
 * mg_heap.h — Heap object enumeration and ObjC identification
 *
 * Enumerates all malloc zones, identifies ObjC/Swift objects
 * via isa pointer validation, and aggregates class statistics.
 */

#ifndef MG_HEAP_H
#define MG_HEAP_H

#include "mg_pool.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — full definition in mg_ref_graph.h. */
typedef struct mg_ref_graph mg_ref_graph_t;

/* Per-class aggregated statistics. */
typedef struct {
    const char *class_name;   /* pointer from class_getName — stable */
    uint32_t    count;
    uint64_t    total_size;
} mg_class_stat_t;

/* Per-zone summary. */
typedef struct {
    const char *name;
    uint64_t    allocated;
    uint64_t    blocks;
} mg_zone_stat_t;

#define MG_MAX_ZONES 32

/* ------------------------------------------------------------------ */
/* Buffer table — non-ObjC malloc blocks for indirect reference scan   */
/* ------------------------------------------------------------------ */

/* A recorded non-ObjC malloc allocation (potential collection buffer). */
typedef struct mg_buffer_entry {
    uint64_t addr;
    uint32_t size;
} mg_buffer_entry_t;

/* Sorted table of non-ObjC buffers for binary-search lookup. */
typedef struct mg_buffer_table {
    mg_buffer_entry_t *entries;
    uint32_t           count;
    uint32_t           capacity;
} mg_buffer_table_t;

/*
 * Lookup a pointer value in the buffer table.
 * Returns the entry if `addr` falls within a recorded buffer, or NULL.
 */
const mg_buffer_entry_t *mg_buffer_table_lookup(const mg_buffer_table_t *bt,
                                                 uint64_t addr);

/* ------------------------------------------------------------------ */
/* Raw (C/C++) allocation statistics                                    */
/* ------------------------------------------------------------------ */

#define MG_RAW_BUCKET_COUNT  6
/* Bucket ranges: [16,256) [256,1K) [1K,4K) [4K,16K) [16K,256K) [256K+) */

typedef struct {
    uint64_t raw_total_size;                        /* total bytes in non-ObjC allocs */
    uint32_t raw_size_buckets[MG_RAW_BUCKET_COUNT]; /* count per size range           */
} mg_raw_alloc_stat_t;

/* Complete heap snapshot. */
typedef struct {
    mg_zone_stat_t  zones[MG_MAX_ZONES];
    uint32_t        zone_count;

    uint64_t        total_allocated;
    uint64_t        total_blocks;
    uint64_t        total_objects;     /* recognized ObjC objects */
    uint64_t        raw_allocations;   /* non-ObjC allocations   */

    mg_raw_alloc_stat_t raw_stat;     /* C/C++ allocation breakdown */

    mg_class_stat_t *top_classes;      /* sorted desc by total_size */
    uint32_t         top_count;

    mg_buffer_table_t *buffer_table;  /* non-ObjC buffers (for indirect ref scan) */
} mg_heap_snapshot_t;

/*
 * Opaque handle for the set of valid ObjC class pointers.
 * Built before thread suspension, used during enumeration.
 */
typedef struct mg_class_set mg_class_set_t;

/*
 * Build a sorted set of all registered ObjC class pointers.
 * Call this BEFORE suspending threads.
 * Returns NULL on allocation failure.
 */
mg_class_set_t *mg_class_set_build(mg_pool_t *pool);

/*
 * Enumerate all malloc zones and their allocations.
 * For each allocation, attempts ObjC object identification using
 * the pre-built class_set.
 *
 * Call this BEFORE suspending other threads.  Zone introspect
 * enumerators acquire internal zone locks; calling while other
 * threads are suspended can deadlock if a suspended thread holds
 * such a lock (common with libpas zones on iOS 15+).
 *
 * top_n:     number of top classes to report (by total_size).
 * ref_graph: optional reference graph builder (NULL to skip).
 *            When provided, each identified ObjC object is also
 *            registered for reference analysis.
 * Returns 0 on success.
 */
int mg_heap_collect(mg_pool_t *pool,
                    const mg_class_set_t *class_set,
                    uint32_t top_n,
                    mg_heap_snapshot_t *out,
                    mg_ref_graph_t *ref_graph);

#endif /* MG_HEAP_H */
