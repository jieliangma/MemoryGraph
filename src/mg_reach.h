/*
 * mg_reach.h — Reachability-based leak detection
 *
 * Identifies GC roots (thread stacks, registers, DATA segments),
 * then runs BFS over the reference graph to find unreachable objects.
 *
 * All memory comes from the pre-allocated pool — no malloc.
 */

#ifndef MG_REACH_H
#define MG_REACH_H

#include "mg_pool.h"
#include "mg_ref_graph.h"
#include "mg_vm_region.h"
#include "mg_thread.h"

#include <stdint.h>
#include <stdbool.h>

/* One class of unreachable objects. */
typedef struct {
    const char *class_name;
    uint32_t    count;
    uint64_t    total_size;
} mg_reach_suspect_t;

/* Reachability analysis result. */
typedef struct {
    mg_reach_suspect_t *suspects;
    uint32_t            suspect_count;
    uint32_t            total_reachable;
    uint32_t            total_unreachable;
    uint32_t            root_count;        /* objects directly referenced by roots */
} mg_reach_result_t;

/*
 * Phase C (threads frozen): scan GC roots and mark root objects.
 *
 * Scans thread stacks, registers, and writable DATA segments for
 * pointers to objects in the reference graph.
 *
 * Returns a bitset of root object indices (pool-allocated).
 * root_bits[i/8] & (1 << (i%8)) is set if object i is a root.
 *
 * Returns 0 on success, negative on error.
 */
int mg_reach_scan_roots(mg_pool_t *pool,
                        const mg_ref_graph_t *graph,
                        const mg_vm_snapshot_t *vm_snap,
                        const mg_thread_state_t *threads,
                        uint8_t **root_bits,
                        uint32_t *root_count);

/*
 * Phase E (threads running): BFS reachability + aggregation.
 *
 * Starting from root_bits, traverses the reference graph's adjacency
 * list.  Unreachable objects are grouped by class name.
 *
 * Returns 0 on success, negative on error.
 */
int mg_reach_analyze(mg_pool_t *pool,
                     const mg_ref_graph_t *graph,
                     const uint8_t *root_bits,
                     mg_reach_result_t *out);

#endif /* MG_REACH_H */
