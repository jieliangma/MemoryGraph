/*
 * mg_ref_graph.h — Reference graph analysis for retain cycle detection
 *
 * Scans ObjC heap objects for inter-object pointer references,
 * builds a directed graph, and detects retain cycles using
 * Tarjan's strongly connected components algorithm.
 *
 * All memory comes from the pre-allocated pool — no malloc.
 */

#ifndef MG_REF_GRAPH_H
#define MG_REF_GRAPH_H

#include "mg_pool.h"
#include <stdint.h>
#include <stdbool.h>

/* Object flags (bitfield). */
#define MG_OBJ_FLAG_BLOCK  (1u << 0)  /* ObjC block (__NSMallocBlock__ etc.) */

/* A recorded ObjC object for reference analysis. */
typedef struct {
    uint64_t    address;
    uint32_t    size;
    const char *class_name;
    uint8_t     flags;
} mg_ref_obj_t;

/* ------------------------------------------------------------------ */
/* Edge packing: target (18) + offset (13) + indirect flag (1) = 32    */
/* ------------------------------------------------------------------ */

#define MG_EDGE_TARGET_BITS   18
#define MG_EDGE_OFFSET_BITS   13
#define MG_EDGE_TARGET_MAX    ((1u << MG_EDGE_TARGET_BITS) - 1)   /* 262143 */
#define MG_EDGE_OFFSET_MAX    ((1u << MG_EDGE_OFFSET_BITS) - 1)   /* 8191   */
#define MG_EDGE_INDIRECT_BIT  (1u << 31)

#define MG_EDGE_PACK(target, offset) \
    ((uint32_t)(((uint32_t)((offset) & MG_EDGE_OFFSET_MAX) << MG_EDGE_TARGET_BITS) | \
     ((target) & MG_EDGE_TARGET_MAX)))

/* Pack an indirect (through-buffer) edge. */
#define MG_EDGE_PACK_INDIRECT(target, offset) \
    (MG_EDGE_PACK(target, offset) | MG_EDGE_INDIRECT_BIT)

#define MG_EDGE_TARGET(e)    ((uint32_t)(e) & MG_EDGE_TARGET_MAX)
#define MG_EDGE_OFFSET(e)    (((uint32_t)(e) >> MG_EDGE_TARGET_BITS) & MG_EDGE_OFFSET_MAX)
#define MG_EDGE_IS_INDIRECT(e) (((uint32_t)(e) & MG_EDGE_INDIRECT_BIT) != 0)

/* Sentinel offset for associated-object virtual edges. */
#define MG_EDGE_ASSOC_OFFSET  MG_EDGE_OFFSET_MAX

/* ------------------------------------------------------------------ */
/* Cycle representation                                                */
/* ------------------------------------------------------------------ */

/* Edge type classification for retain cycle edges. */
typedef enum {
    MG_EDGE_IVAR       = 0,   /* normal ivar/property reference */
    MG_EDGE_BLOCK      = 1,   /* captured variable in an ObjC block */
    MG_EDGE_COLLECTION = 2,   /* element in a collection (indirect) */
    MG_EDGE_ASSOCIATED = 3,   /* associated object (objc_setAssociatedObject) */
    MG_EDGE_UNKNOWN    = 4,   /* cannot determine */
} mg_edge_type_t;

/* One step in a directed retain cycle path. */
typedef struct {
    const char    *from_class;
    const char    *ivar_name;   /* resolved name, or label like "block_capture" */
    const char    *to_class;
    mg_edge_type_t edge_type;
} mg_cycle_step_t;

/* A detected retain cycle. */
typedef struct {
    mg_cycle_step_t *steps;          /* ordered: steps[i].to == steps[i+1].from */
    uint32_t         length;         /* number of steps == number of objects     */
    uint32_t         instance_count; /* how many SCC instances match this pattern */
    uint64_t         total_size;     /* cumulative size across all instances      */
    bool             is_app_cycle;   /* true if >=1 class is non-system           */
    const char     **sorted_names;   /* for dedup (not emitted in report)         */
    uint32_t         name_hash;      /* FNV-1a of sorted names (fast dedup)       */
} mg_retain_cycle_t;

/* Options for cycle detection. */
typedef struct {
    bool filter_system_cycles;  /* skip cycles where ALL classes are system-level */
} mg_ref_graph_options_t;

/* Reference graph analysis result. */
typedef struct {
    bool              enabled;
    uint32_t          total_nodes;
    uint32_t          total_edges;
    mg_retain_cycle_t *cycles;
    uint32_t           cycle_count;
    /* Truncation telemetry */
    uint32_t          objects_dropped;
    uint32_t          edges_dropped;
    uint32_t          cycles_dropped_length;
    uint32_t          cycles_dropped_limit;
    uint32_t          system_cycles_filtered;
} mg_ref_graph_result_t;

/* Opaque handle for the graph builder. */
typedef struct mg_ref_graph mg_ref_graph_t;

/*
 * Create a reference graph builder.
 * max_objects: capacity for object recording.
 * Returns NULL on allocation failure.
 */
mg_ref_graph_t *mg_ref_graph_create(mg_pool_t *pool, uint32_t max_objects);

/*
 * Register an ObjC object discovered during heap enumeration.
 * Called from the heap enumerator callback.
 * flags: bitfield — use MG_OBJ_FLAG_BLOCK for block objects.
 */
void mg_ref_graph_add_object(mg_ref_graph_t *g,
                             uint64_t address,
                             uint32_t size,
                             const char *class_name,
                             uint8_t flags);

/*
 * Build the address lookup index after all objects have been added.
 * Must be called before mg_ref_graph_scan_references().
 */
void mg_ref_graph_build_index(mg_ref_graph_t *g);

/*
 * Scan all recorded objects' memory for pointers to other recorded
 * objects, building the reference edge list.
 *
 * buffer_table: optional — when provided, enables indirect reference
 *               scanning through non-ObjC malloc buffers (e.g. NSArray
 *               backing stores).  Pointers through buffers are recorded
 *               with the source object's offset to the buffer pointer.
 *               Pass NULL to disable indirect scanning.
 *               (struct mg_buffer_table_t is defined in mg_heap.h.)
 *
 * MUST be called while other threads are suspended (reads raw memory).
 */
struct mg_buffer_table;  /* forward declaration — full def in mg_heap.h */
void mg_ref_graph_scan_references(mg_ref_graph_t *g,
                                  const struct mg_buffer_table *buffer_table);

/*
 * Inject virtual edges from associated object tracking.
 * For each (object, value) pair where both addresses are known
 * objects in the graph, adds an edge marked as MG_EDGE_ASSOCIATED.
 *
 * Call AFTER mg_ref_graph_scan_references() and BEFORE find_cycles().
 * entries/count come from mg_assoc_tracker_snapshot().
 */
struct mg_assoc_entry;  /* forward declaration — full def in mg_assoc_tracker.h */
void mg_ref_graph_inject_associations(mg_ref_graph_t *g,
                                      const struct mg_assoc_entry *entries,
                                      uint32_t count);

/* Forward-declare ivar map (see mg_ivar_map.h). */
typedef struct mg_ivar_map mg_ivar_map_t;

/*
 * Find retain cycles via Tarjan's SCC algorithm.
 * Can be called after threads are resumed.
 *
 * ivar_map: optional — provides ivar name resolution for edges.
 * opts:     optional — NULL uses defaults (system filtering on).
 *
 * Writes results into `out`; cycle data is pool-allocated.
 * Returns 0 on success.
 */
int mg_ref_graph_find_cycles(mg_pool_t *pool,
                             mg_ref_graph_t *g,
                             const mg_ivar_map_t *ivar_map,
                             const mg_ref_graph_options_t *opts,
                             mg_ref_graph_result_t *out);

/* ------------------------------------------------------------------ */
/* Read-only accessors (for reachability analysis)                     */
/* ------------------------------------------------------------------ */

/*
 * Look up an address in the graph's hash table.
 * Returns the object index, or -1 if not found.
 * The index must only be called after mg_ref_graph_build_index().
 */
int32_t mg_ref_graph_lookup(const mg_ref_graph_t *g, uint64_t address);

/*
 * Read-only access to the object array and CSR adjacency lists.
 */
const mg_ref_obj_t *mg_ref_graph_objects(const mg_ref_graph_t *g,
                                          uint32_t *count);
const uint32_t *mg_ref_graph_adj_offsets(const mg_ref_graph_t *g);
const uint32_t *mg_ref_graph_adj_targets(const mg_ref_graph_t *g);

#endif /* MG_REF_GRAPH_H */
