/*
 * mg_ref_graph.c — Reference graph analysis for retain cycle detection
 *
 * 1. Records ObjC objects during heap enumeration (address, size, class).
 * 2. Builds an address hash table for O(1) lookups.
 * 3. Scans each object's memory for pointer-sized values pointing to
 *    other known objects (must run while threads are suspended).
 *    - Filters tagged pointers and unaligned values to reduce false edges.
 *    - Packs source byte offset into each edge for ivar name resolution.
 * 4. Runs iterative Tarjan's SCC to detect retain cycles.
 *    - Deduplicates cycles by sorted class names, tracks instance counts.
 *    - Extracts directed cycle paths with ivar name annotations.
 *    - Filters system-only cycles; sorts by impact (total_size).
 *
 * No malloc/free — all scratch memory comes from the pre-allocated pool.
 */

#include "mg_ref_graph.h"
#include "mg_heap.h"
#include "mg_assoc_tracker.h"
#include "mg_ivar_map.h"
#include "../include/memory_graph.h"
#include <string.h>
#include <stdlib.h>    /* qsort */

#if defined(__arm64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

_Static_assert(200000 <= MG_EDGE_TARGET_MAX,
               "max_objs cap exceeds edge target bit width");

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define MG_MAX_EDGES_PER_OBJ  256   /* max outgoing refs scanned per obj */
#define MG_MAX_CYCLES_REPORT  100   /* max unique retain cycles to report */
#define MG_MAX_CYCLE_LENGTH    20   /* max objects in a single cycle     */

/* ------------------------------------------------------------------ */
/* Address hash table slot                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t addr;      /* 0 = empty slot */
    uint32_t obj_idx;
} addr_slot_t;

/* ------------------------------------------------------------------ */
/* Graph structure                                                     */
/* ------------------------------------------------------------------ */

struct mg_ref_graph {
    mg_pool_t    *pool;

    /* Object storage */
    mg_ref_obj_t *objects;
    uint32_t      obj_count;
    uint32_t      obj_capacity;

    /* Address hash table */
    addr_slot_t  *addr_map;
    uint32_t      addr_map_mask;   /* capacity - 1 */

    /* Adjacency (CSR format, built during scan) */
    uint32_t     *adj_offsets;     /* [obj_capacity + 1] */
    uint32_t     *adj_targets;     /* packed: MG_EDGE_PACK(target, offset) */
    uint32_t      edge_count;
    uint32_t      edge_capacity;

    /* Truncation counters */
    uint32_t      objects_dropped;
    uint32_t      edges_dropped;
};

/* ------------------------------------------------------------------ */
/* Hash helper                                                         */
/* ------------------------------------------------------------------ */

static inline uint32_t hash_addr(uint64_t a) {
    a ^= a >> 33;
    a *= 0xff51afd7ed558ccdULL;
    a ^= a >> 33;
    return (uint32_t)a;
}

/* ------------------------------------------------------------------ */
/* Create                                                              */
/* ------------------------------------------------------------------ */

mg_ref_graph_t *mg_ref_graph_create(mg_pool_t *pool, uint32_t max_objects) {
    if (!pool || max_objects == 0) return NULL;

    mg_ref_graph_t *g = (mg_ref_graph_t *)mg_pool_alloc(
        pool, sizeof(mg_ref_graph_t));
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->pool         = pool;
    g->obj_capacity = max_objects;

    /* Object array. */
    g->objects = (mg_ref_obj_t *)mg_pool_alloc(
        pool, sizeof(mg_ref_obj_t) * max_objects);
    if (!g->objects) return NULL;

    /* Address hash table — 2x objects, power of 2. */
    uint32_t map_cap = 1;
    while (map_cap < max_objects * 2) map_cap <<= 1;
    g->addr_map_mask = map_cap - 1;
    g->addr_map = (addr_slot_t *)mg_pool_alloc(
        pool, sizeof(addr_slot_t) * map_cap);
    if (!g->addr_map) return NULL;
    memset(g->addr_map, 0, sizeof(addr_slot_t) * map_cap);

    /* Adjacency offsets. */
    g->adj_offsets = (uint32_t *)mg_pool_alloc(
        pool, sizeof(uint32_t) * (max_objects + 1));
    if (!g->adj_offsets) return NULL;
    memset(g->adj_offsets, 0, sizeof(uint32_t) * (max_objects + 1));

    /* Edge target array — estimate avg 4 edges per object. */
    g->edge_capacity = max_objects * 4;
    g->adj_targets = (uint32_t *)mg_pool_alloc(
        pool, sizeof(uint32_t) * g->edge_capacity);
    if (!g->adj_targets) return NULL;

    return g;
}

/* ------------------------------------------------------------------ */
/* Add object                                                          */
/* ------------------------------------------------------------------ */

void mg_ref_graph_add_object(mg_ref_graph_t *g,
                             uint64_t address,
                             uint32_t size,
                             const char *class_name,
                             uint8_t flags) {
    if (!g) return;
    if (g->obj_count >= g->obj_capacity) {
        g->objects_dropped++;
        return;
    }

    uint32_t idx = g->obj_count;
    g->objects[idx].address    = address;
    g->objects[idx].size       = size;
    g->objects[idx].class_name = class_name;
    g->objects[idx].flags      = flags;
    g->obj_count++;
}

/* ------------------------------------------------------------------ */
/* Build address index                                                 */
/* ------------------------------------------------------------------ */

void mg_ref_graph_build_index(mg_ref_graph_t *g) {
    if (!g) return;

    for (uint32_t i = 0; i < g->obj_count; i++) {
        uint64_t addr = g->objects[i].address;
        uint32_t slot = hash_addr(addr) & g->addr_map_mask;

        while (g->addr_map[slot].addr != 0) {
            slot = (slot + 1) & g->addr_map_mask;
        }
        g->addr_map[slot].addr    = addr;
        g->addr_map[slot].obj_idx = i;
    }
}

/* Lookup: returns object index or UINT32_MAX if not found. */
static uint32_t addr_lookup(const mg_ref_graph_t *g, uint64_t addr) {
    if (addr == 0) return UINT32_MAX;
    uint32_t slot = hash_addr(addr) & g->addr_map_mask;

    while (g->addr_map[slot].addr != 0) {
        if (g->addr_map[slot].addr == addr)
            return g->addr_map[slot].obj_idx;
        slot = (slot + 1) & g->addr_map_mask;
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/* Scan references                                                     */
/* ------------------------------------------------------------------ */

/* Try to record an edge from object `src_idx` at byte offset `off`
 * pointing to `val`.  Returns 1 if an edge was added, 0 otherwise. */
static inline uint32_t try_add_edge(mg_ref_graph_t *g, uint32_t src_idx,
                                    uint64_t src_addr, uint64_t p,
                                    uintptr_t val) {
    uint32_t target = addr_lookup(g, (uint64_t)val);
    if (target != UINT32_MAX && target != src_idx) {
        if (g->edge_count < g->edge_capacity) {
            uint32_t off = (uint32_t)(p - src_addr);
            g->adj_targets[g->edge_count++] =
                MG_EDGE_PACK(target, off);
            return 1;
        }
        g->edges_dropped++;
    }
    return 0;
}

/* Max buffers to penetrate per object (limits scan time). */
#define MG_MAX_BUFFERS_PER_OBJ  8

/* Inline pointer validity check (shared between NEON remainder and scalar). */
static inline bool is_valid_pointer(uintptr_t val) {
    if (val == 0) return false;
#if defined(__arm64__)
    if (val & 0x8000000000000000ULL) return false;
#elif defined(__x86_64__)
    if (val & 0x1ULL) return false;
#endif
    if (val & 0x7ULL) return false;
    return true;
}

/*
 * Scan a buffer's contents for pointers to known ObjC objects.
 * Records indirect edges using `src_off` (the offset in the source
 * ObjC object where the buffer pointer was found).
 * Returns the number of edges added.
 */
static uint32_t scan_buffer_for_objects(mg_ref_graph_t *g,
                                        uint32_t src_idx,
                                        uint32_t src_off,
                                        const mg_buffer_entry_t *buf) {
    uint32_t added = 0;
    uint64_t bp = buf->addr;
    uint64_t bp_end = bp + buf->size;

    for (; bp + sizeof(void *) <= bp_end; bp += sizeof(void *)) {
        uintptr_t val = *(uintptr_t *)bp;
        if (!is_valid_pointer(val)) continue;

        uint32_t target = addr_lookup(g, (uint64_t)val);
        if (target != UINT32_MAX && target != src_idx) {
            if (g->edge_count < g->edge_capacity) {
                /* Use source object offset (not buffer offset) so
                 * ivar resolution points to the collection ivar.
                 * Mark as indirect so cycle extraction knows it
                 * traversed a buffer. */
                g->adj_targets[g->edge_count++] =
                    MG_EDGE_PACK_INDIRECT(target, src_off);
                added++;
            } else {
                g->edges_dropped++;
            }
        }
    }
    return added;
}

void mg_ref_graph_scan_references(mg_ref_graph_t *g,
                                  const mg_buffer_table_t *buffer_table) {
    if (!g || g->obj_count == 0) return;

    for (uint32_t i = 0; i < g->obj_count; i++) {
        g->adj_offsets[i] = g->edge_count;

        uint64_t addr = g->objects[i].address;
        uint32_t size = g->objects[i].size;

        /* Skip isa pointer; scan remaining pointer-sized slots. */
        uint64_t scan_start = addr + sizeof(void *);
        uint64_t scan_end   = addr + size;
        uint32_t edges_this = 0;
        uint32_t buffers_scanned = 0;
        uint64_t p = scan_start;

#if defined(__arm64__) && defined(__ARM_NEON)
        /*
         * NEON fast path: load 2 pointers (128 bits) per iteration.
         * Filter zero, tagged (bit 63), and unaligned (bits 0-2) values
         * in parallel using vector bitwise ops.  Only call addr_lookup()
         * for candidates that survive all three filters.
         */
        const uint64x2_t tag_mask   = vdupq_n_u64(0x8000000000000000ULL);
        const uint64x2_t align_mask = vdupq_n_u64(0x7ULL);
        const uint64x2_t zero       = vdupq_n_u64(0);

        for (; p + 16 <= scan_end && edges_this < MG_MAX_EDGES_PER_OBJ;
             p += 16)
        {
            uint64x2_t vec = vld1q_u64((const uint64_t *)p);

            /* bad = (val == 0) | (val & tag) | (val & align) */
            uint64x2_t is_zero = vceqq_u64(vec, zero);
            uint64x2_t is_tag  = vtstq_u64(vec, tag_mask);
            uint64x2_t is_unal = vtstq_u64(vec, align_mask);
            uint64x2_t bad     = vorrq_u64(vorrq_u64(is_zero, is_tag), is_unal);

            /* Extract per-lane results.  vgetq_lane_u64 compiles to a
             * single UMOV — no vector-to-GPR penalty on Apple Silicon. */
            for (int lane = 0; lane < 2; lane++) {
                uint64_t bad_lane = lane == 0 ?
                    vgetq_lane_u64(bad, 0) : vgetq_lane_u64(bad, 1);
                if (bad_lane) continue;

                uintptr_t val = (uintptr_t)(lane == 0 ?
                    vgetq_lane_u64(vec, 0) : vgetq_lane_u64(vec, 1));
                uint64_t slot_p = p + (uint64_t)lane * 8;

                uint32_t direct = try_add_edge(g, i, addr, slot_p, val);
                edges_this += direct;

                /* Penetrating scan: if not a direct object ref, try buffer. */
                if (!direct && buffer_table &&
                    buffers_scanned < MG_MAX_BUFFERS_PER_OBJ)
                {
                    const mg_buffer_entry_t *buf =
                        mg_buffer_table_lookup(buffer_table, (uint64_t)val);
                    if (buf) {
                        uint32_t off = (uint32_t)(slot_p - addr);
                        edges_this += scan_buffer_for_objects(
                            g, i, off, buf);
                        buffers_scanned++;
                    }
                }
            }
        }
#endif /* __arm64__ && __ARM_NEON */

        /* Scalar tail: handles remaining bytes and x86_64. */
        for (; p + sizeof(void *) <= scan_end &&
               edges_this < MG_MAX_EDGES_PER_OBJ;
             p += sizeof(void *))
        {
            uintptr_t val = *(uintptr_t *)p;
            if (!is_valid_pointer(val)) continue;

            uint32_t direct = try_add_edge(g, i, addr, p, val);
            edges_this += direct;

            /* Penetrating scan: if not a direct object ref, try buffer. */
            if (!direct && buffer_table &&
                buffers_scanned < MG_MAX_BUFFERS_PER_OBJ)
            {
                const mg_buffer_entry_t *buf =
                    mg_buffer_table_lookup(buffer_table, (uint64_t)val);
                if (buf) {
                    uint32_t off = (uint32_t)(p - addr);
                    edges_this += scan_buffer_for_objects(
                        g, i, off, buf);
                    buffers_scanned++;
                }
            }
        }
    }
    g->adj_offsets[g->obj_count] = g->edge_count;
}

/* ------------------------------------------------------------------ */
/* Inject associated-object virtual edges                              */
/* ------------------------------------------------------------------ */

void mg_ref_graph_inject_associations(mg_ref_graph_t *g,
                                      const mg_assoc_entry_t *entries,
                                      uint32_t count) {
    if (!g || !entries || count == 0) return;

    /* adj_offsets has already been finalized by scan_references.
     * We append edges to the end.  The adj_offsets CSR structure
     * can't be updated incrementally, so we must rebuild it after
     * injection.  For simplicity, we just append to the flat edge
     * array and let find_cycles use a secondary edge range.
     *
     * Actually, the simpler approach: bump the last adj_offsets entry
     * won't work for CSR.  Instead, store associated edges separately
     * and check them during cycle extraction.
     *
     * Simplest correct approach: append edges to adj_targets and
     * rebuild adj_offsets.  Since we're post-scan (threads resumed),
     * we have time. */

    /* First pass: count how many valid edges we'll add per object. */
    uint32_t *extra_edges = (uint32_t *)mg_pool_alloc(
        g->pool, sizeof(uint32_t) * g->obj_count);
    if (!extra_edges) return;
    memset(extra_edges, 0, sizeof(uint32_t) * g->obj_count);

    uint32_t valid = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t src = addr_lookup(g, (uint64_t)entries[i].object);
        uint32_t tgt = addr_lookup(g, (uint64_t)entries[i].value);
        if (src != UINT32_MAX && tgt != UINT32_MAX && src != tgt) {
            extra_edges[src]++;
            valid++;
        }
    }

    if (valid == 0) return;

    /* Allocate a new, larger edge array. */
    uint32_t new_capacity = g->edge_count + valid;
    uint32_t *new_targets = (uint32_t *)mg_pool_alloc(
        g->pool, sizeof(uint32_t) * new_capacity);
    if (!new_targets) return;

    /* Rebuild adj_offsets with extra space for associated edges.
     * Copy existing edges, leaving gaps for the new ones. */
    uint32_t *new_offsets = (uint32_t *)mg_pool_alloc(
        g->pool, sizeof(uint32_t) * (g->obj_count + 1));
    if (!new_offsets) return;

    /* Compute new offsets. */
    uint32_t pos = 0;
    for (uint32_t i = 0; i < g->obj_count; i++) {
        new_offsets[i] = pos;
        uint32_t existing = g->adj_offsets[i + 1] - g->adj_offsets[i];
        /* Copy existing edges for this object. */
        memcpy(new_targets + pos, g->adj_targets + g->adj_offsets[i],
               sizeof(uint32_t) * existing);
        pos += existing + extra_edges[i];
    }
    new_offsets[g->obj_count] = pos;

    /* Second pass: insert associated edges into the gaps. */
    /* Track fill position per object (start at end of copied existing edges). */
    uint32_t *fill = extra_edges; /* reuse buffer */
    for (uint32_t i = 0; i < g->obj_count; i++) {
        uint32_t existing = g->adj_offsets[i + 1] - g->adj_offsets[i];
        fill[i] = new_offsets[i] + existing;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t src = addr_lookup(g, (uint64_t)entries[i].object);
        uint32_t tgt = addr_lookup(g, (uint64_t)entries[i].value);
        if (src != UINT32_MAX && tgt != UINT32_MAX && src != tgt) {
            new_targets[fill[src]++] =
                MG_EDGE_PACK(tgt, MG_EDGE_ASSOC_OFFSET);
        }
    }

    /* Replace old arrays. */
    g->adj_targets  = new_targets;
    g->adj_offsets  = new_offsets;
    g->edge_count   = new_capacity;
    g->edge_capacity = new_capacity;
}

/* ------------------------------------------------------------------ */
/* System class predicate                                              */
/* ------------------------------------------------------------------ */

static bool is_system_class(const char *name) {
    if (!name || name[0] == '\0') return false;
    if (name[0] == '_')  return true;   /* private Apple classes */

    static const char * const prefixes[] = {
        "NS", "UI", "CF", "CA", "CK", "AV", "CL",
        "MK", "WK", "OS_", "XC", "BK", "BS", "FB",
        "RB", "AX", "CX", "CS", "CT", "SC", "MT",
        "PK", "GK", "HK", "SK", "QL", "TN", NULL
    };
    for (const char * const *p = prefixes; *p; p++) {
        size_t len = strlen(*p);
        if (strncmp(name, *p, len) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Tarjan's SCC (iterative — avoids stack overflow on large graphs)    */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t  index;     /* -1 = unvisited */
    int32_t  lowlink;
    bool     on_stack;
} tarjan_node_t;

/* One frame on the explicit call stack. */
typedef struct {
    uint32_t v;
    uint32_t edge_idx;  /* next edge to process */
    uint32_t child;     /* child we descended into */
    bool     returned;  /* returning from a child */
} tarjan_frame_t;

typedef struct {
    mg_ref_graph_t *g;
    tarjan_node_t  *nodes;
    uint32_t       *scc_stack;      /* Tarjan DFS stack */
    uint32_t        scc_top;
    int32_t         index_counter;

    tarjan_frame_t *call_stack;
    uint32_t        call_stack_cap;

    /* Results */
    mg_pool_t              *pool;
    const mg_ivar_map_t    *ivar_map;       /* nullable */
    bool                    filter_system;
    mg_retain_cycle_t      *cycles;
    uint32_t                cycle_count;
    uint32_t                max_cycles;

    /* Scratch for path extraction — allocated once, reused */
    uint8_t                *scc_member;     /* bitset: in current SCC */
    uint8_t                *visited;        /* bitset: visited in path DFS */
    uint32_t                bitset_bytes;

    /* Truncation counters */
    uint32_t  cycles_dropped_length;
    uint32_t  cycles_dropped_limit;
    uint32_t  system_cycles_filtered;
} tarjan_ctx_t;

/* ------------------------------------------------------------------ */
/* Bitset helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void bset(uint8_t *bits, uint32_t i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static inline bool btest(const uint8_t *bits, uint32_t i) {
    return (bits[i >> 3] & (1u << (i & 7))) != 0;
}

/* ------------------------------------------------------------------ */
/* Sort class names for dedup                                          */
/* ------------------------------------------------------------------ */

/* Insertion sort — n is bounded by MG_MAX_CYCLE_LENGTH (20), so O(n^2)
 * is faster than qsort() due to lower call overhead and branch prediction. */
static void sort_names(const char **a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        const char *key = a[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && strcmp(a[j], key) > 0) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/* FNV-1a hash of sorted class name pointers for fast cycle dedup. */
static uint32_t hash_sorted_names(const char **names, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        const char *s = names[i];
        for (; *s; s++) {
            h ^= (uint32_t)(unsigned char)*s;
            h *= 16777619u;
        }
        h ^= 0xFF;  /* separator between names */
        h *= 16777619u;
    }
    return h;
}

/* Returns index of matching cycle, or -1 if none. */
static int32_t find_duplicate_cycle(const tarjan_ctx_t *ctx,
                                    const char **sorted_names,
                                    uint32_t len,
                                    uint32_t name_hash) {
    for (uint32_t c = 0; c < ctx->cycle_count; c++) {
        if (ctx->cycles[c].length != len) continue;
        if (ctx->cycles[c].name_hash != name_hash) continue;
        bool match = true;
        for (uint32_t i = 0; i < len; i++) {
            if (strcmp(sorted_names[i],
                       ctx->cycles[c].sorted_names[i]) != 0) {
                match = false;
                break;
            }
        }
        if (match) return (int32_t)c;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Cycle path extraction — DFS restricted to SCC members               */
/* ------------------------------------------------------------------ */

/* Trace one directed cycle through an SCC.  Returns the number of
 * steps written to `steps[]`, or 0 on failure.  `steps` must have
 * room for MG_MAX_CYCLE_LENGTH entries. */
static uint32_t extract_cycle_path(tarjan_ctx_t *ctx,
                                   const uint32_t *scc_nodes,
                                   uint32_t scc_size,
                                   mg_cycle_step_t *steps) {
    mg_ref_graph_t *g = ctx->g;

    /* Mark SCC membership. */
    memset(ctx->scc_member, 0, ctx->bitset_bytes);
    for (uint32_t i = 0; i < scc_size; i++)
        bset(ctx->scc_member, scc_nodes[i]);

    /* DFS path stack (local, small). */
    uint32_t path_nodes[MG_MAX_CYCLE_LENGTH + 1];
    uint32_t path_edges[MG_MAX_CYCLE_LENGTH + 1]; /* packed edges */
    uint32_t path_len = 0;

    memset(ctx->visited, 0, ctx->bitset_bytes);

    uint32_t start = scc_nodes[0];
    path_nodes[0] = start;
    bset(ctx->visited, start);

    uint32_t current = start;

    /* Iterative DFS with backtracking. */
    /* edge_cursor[depth] tracks which edge index to try next. */
    uint32_t edge_cursor[MG_MAX_CYCLE_LENGTH + 1];
    edge_cursor[0] = g->adj_offsets[current];

    while (true) {
        uint32_t edge_end = g->adj_offsets[current + 1];
        bool advanced = false;

        while (edge_cursor[path_len] < edge_end) {
            uint32_t packed = g->adj_targets[edge_cursor[path_len]];
            edge_cursor[path_len]++;

            uint32_t w = MG_EDGE_TARGET(packed);
            if (!btest(ctx->scc_member, w)) continue;

            if (w == start && path_len >= 1) {
                /* Cycle complete: record final edge and return. */
                path_edges[path_len] = packed;
                path_len++;

                /* Build steps with edge type classification. */
                for (uint32_t i = 0; i < path_len; i++) {
                    uint32_t src_idx = path_nodes[i];
                    uint32_t pe      = path_edges[i];
                    uint32_t tgt_idx = MG_EDGE_TARGET(pe);
                    uint16_t off     = (uint16_t)MG_EDGE_OFFSET(pe);
                    bool indirect    = MG_EDGE_IS_INDIRECT(pe);

                    steps[i].from_class = g->objects[src_idx].class_name;
                    steps[i].to_class   = g->objects[tgt_idx].class_name;
                    steps[i].ivar_name  = NULL;
                    steps[i].edge_type  = MG_EDGE_UNKNOWN;

                    if (off == MG_EDGE_ASSOC_OFFSET) {
                        /* Virtual edge from associated object tracking. */
                        steps[i].ivar_name = "associated_object";
                        steps[i].edge_type = MG_EDGE_ASSOCIATED;
                    } else if (indirect) {
                        /* Edge went through a non-ObjC buffer
                         * (collection backing store). */
                        steps[i].edge_type = MG_EDGE_COLLECTION;
                        if (ctx->ivar_map) {
                            steps[i].ivar_name = mg_ivar_map_lookup(
                                ctx->ivar_map,
                                g->objects[src_idx].class_name, off);
                        }
                        if (!steps[i].ivar_name)
                            steps[i].ivar_name = "collection";
                    } else if (g->objects[src_idx].flags & MG_OBJ_FLAG_BLOCK) {
                        /* Source is a Block — offset >= 32 means
                         * captured variable (past isa/flags/invoke/descriptor). */
                        if (off >= 32) {
                            steps[i].ivar_name = "block_capture";
                            steps[i].edge_type = MG_EDGE_BLOCK;
                        } else {
                            steps[i].ivar_name = "block_internal";
                        }
                    } else if (ctx->ivar_map) {
                        steps[i].ivar_name = mg_ivar_map_lookup(
                            ctx->ivar_map,
                            g->objects[src_idx].class_name, off);
                        if (steps[i].ivar_name)
                            steps[i].edge_type = MG_EDGE_IVAR;
                    }

                    if (!steps[i].ivar_name)
                        steps[i].ivar_name = "?";
                }
                return path_len;
            }

            if (btest(ctx->visited, w)) continue;
            if (path_len >= MG_MAX_CYCLE_LENGTH) continue;

            /* Descend. */
            path_edges[path_len] = packed;
            path_len++;
            path_nodes[path_len] = w;
            bset(ctx->visited, w);
            current = w;
            edge_cursor[path_len] = g->adj_offsets[w];
            advanced = true;
            break;
        }

        if (!advanced) {
            /* Backtrack. */
            if (path_len == 0) return 0;  /* no cycle found */
            path_len--;
            current = path_nodes[path_len];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Emit a detected cycle                                               */
/* ------------------------------------------------------------------ */

static void tarjan_emit_cycle(tarjan_ctx_t *ctx,
                              uint32_t scc_bottom,
                              uint32_t scc_size) {
    /* Length filters. */
    if (scc_size < 2) return;
    if (scc_size > MG_MAX_CYCLE_LENGTH) {
        ctx->cycles_dropped_length++;
        return;
    }
    if (ctx->cycle_count >= ctx->max_cycles) {
        ctx->cycles_dropped_limit++;
        return;
    }

    /* Build sorted class name array for dedup. */
    const char **sorted = (const char **)mg_pool_alloc(
        ctx->pool, sizeof(const char *) * scc_size);
    if (!sorted) return;

    const uint32_t *scc_nodes = &ctx->scc_stack[scc_bottom];

    /* Compute SCC total size. */
    uint64_t scc_total_size = 0;
    bool has_app_class = false;
    for (uint32_t i = 0; i < scc_size; i++) {
        uint32_t node_idx = scc_nodes[i];
        sorted[i] = ctx->g->objects[node_idx].class_name;
        scc_total_size += ctx->g->objects[node_idx].size;
        if (!is_system_class(sorted[i]))
            has_app_class = true;
    }

    sort_names(sorted, scc_size);

    /* System cycle filter. */
    if (ctx->filter_system && !has_app_class) {
        ctx->system_cycles_filtered++;
        return;
    }

    /* Dedup: check if identical cycle pattern already exists. */
    uint32_t nh = hash_sorted_names(sorted, scc_size);
    int32_t dup = find_duplicate_cycle(ctx, sorted, scc_size, nh);
    if (dup >= 0) {
        ctx->cycles[dup].instance_count++;
        ctx->cycles[dup].total_size += scc_total_size;
        return;
    }

    /* Extract directed cycle path. */
    mg_cycle_step_t tmp_steps[MG_MAX_CYCLE_LENGTH];
    uint32_t path_len = extract_cycle_path(ctx, scc_nodes, scc_size,
                                           tmp_steps);
    if (path_len == 0) {
        /* Fallback: couldn't trace cycle (shouldn't happen for valid SCC). */
        return;
    }

    /* Allocate steps from pool. */
    mg_cycle_step_t *steps = (mg_cycle_step_t *)mg_pool_alloc(
        ctx->pool, sizeof(mg_cycle_step_t) * path_len);
    if (!steps) return;
    memcpy(steps, tmp_steps, sizeof(mg_cycle_step_t) * path_len);

    /* Record cycle. */
    mg_retain_cycle_t *cyc = &ctx->cycles[ctx->cycle_count];
    cyc->steps          = steps;
    cyc->length         = path_len;
    cyc->instance_count = 1;
    cyc->total_size     = scc_total_size;
    cyc->is_app_cycle   = has_app_class;
    cyc->sorted_names   = sorted;
    cyc->name_hash      = nh;
    ctx->cycle_count++;
}

/* ------------------------------------------------------------------ */
/* Tarjan main loop                                                    */
/* ------------------------------------------------------------------ */

static void tarjan_run(tarjan_ctx_t *ctx) {
    uint32_t n = ctx->g->obj_count;

    for (uint32_t start = 0; start < n; start++) {
        if (ctx->nodes[start].index != -1) continue;

        /* Push initial frame. */
        int32_t sp = 0;
        ctx->call_stack[0].v         = start;
        ctx->call_stack[0].edge_idx  = ctx->g->adj_offsets[start];
        ctx->call_stack[0].returned  = false;
        ctx->call_stack[0].child     = 0;

        ctx->nodes[start].index   = ctx->index_counter;
        ctx->nodes[start].lowlink = ctx->index_counter;
        ctx->index_counter++;
        ctx->scc_stack[ctx->scc_top++] = start;
        ctx->nodes[start].on_stack = true;

        while (sp >= 0) {
            uint32_t v = ctx->call_stack[sp].v;

            /* Returning from a child? Update lowlink. */
            if (ctx->call_stack[sp].returned) {
                uint32_t w = ctx->call_stack[sp].child;
                if (ctx->nodes[w].lowlink < ctx->nodes[v].lowlink)
                    ctx->nodes[v].lowlink = ctx->nodes[w].lowlink;
                ctx->call_stack[sp].returned = false;
            }

            uint32_t edge_end = ctx->g->adj_offsets[v + 1];
            bool descended = false;

            while (ctx->call_stack[sp].edge_idx < edge_end) {
                uint32_t packed =
                    ctx->g->adj_targets[ctx->call_stack[sp].edge_idx];
                uint32_t w = MG_EDGE_TARGET(packed);
                ctx->call_stack[sp].edge_idx++;

                if (ctx->nodes[w].index == -1) {
                    /* Unvisited — descend. */
                    ctx->call_stack[sp].child = w;

                    sp++;
                    if ((uint32_t)sp >= ctx->call_stack_cap) {
                        sp--;
                        continue;  /* stack limit reached, skip */
                    }

                    ctx->call_stack[sp].v        = w;
                    ctx->call_stack[sp].edge_idx = ctx->g->adj_offsets[w];
                    ctx->call_stack[sp].returned = false;
                    ctx->call_stack[sp].child    = 0;

                    ctx->nodes[w].index   = ctx->index_counter;
                    ctx->nodes[w].lowlink = ctx->index_counter;
                    ctx->index_counter++;
                    ctx->scc_stack[ctx->scc_top++] = w;
                    ctx->nodes[w].on_stack = true;

                    descended = true;
                    break;
                } else if (ctx->nodes[w].on_stack) {
                    if (ctx->nodes[w].index < ctx->nodes[v].lowlink)
                        ctx->nodes[v].lowlink = ctx->nodes[w].index;
                }
            }

            if (!descended) {
                /* All edges processed — check if v is SCC root. */
                if (ctx->nodes[v].lowlink == ctx->nodes[v].index) {
                    uint32_t scc_start = ctx->scc_top;
                    uint32_t w;
                    do {
                        w = ctx->scc_stack[--ctx->scc_top];
                        ctx->nodes[w].on_stack = false;
                    } while (w != v);

                    uint32_t scc_size = scc_start - ctx->scc_top;
                    tarjan_emit_cycle(ctx, ctx->scc_top, scc_size);
                }

                sp--;
                if (sp >= 0) {
                    ctx->call_stack[sp].returned = true;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Impact sort — most severe cycles first                              */
/* ------------------------------------------------------------------ */

static int cmp_cycle_impact(const void *a, const void *b) {
    const mg_retain_cycle_t *ca = (const mg_retain_cycle_t *)a;
    const mg_retain_cycle_t *cb = (const mg_retain_cycle_t *)b;
    if (ca->total_size > cb->total_size) return -1;
    if (ca->total_size < cb->total_size) return  1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public: find cycles                                                 */
/* ------------------------------------------------------------------ */

int mg_ref_graph_find_cycles(mg_pool_t *pool,
                             mg_ref_graph_t *g,
                             const mg_ivar_map_t *ivar_map,
                             const mg_ref_graph_options_t *opts,
                             mg_ref_graph_result_t *out) {
    if (!pool || !g || !out) return MG_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    out->enabled     = true;
    out->total_nodes = g->obj_count;
    out->total_edges = g->edge_count;

    /* Copy graph-level truncation counters. */
    out->objects_dropped = g->objects_dropped;
    out->edges_dropped   = g->edges_dropped;

    if (g->obj_count == 0) return 0;

    /* Allocate Tarjan state. */
    uint32_t n = g->obj_count;

    tarjan_node_t *nodes = (tarjan_node_t *)mg_pool_alloc(
        pool, sizeof(tarjan_node_t) * n);
    if (!nodes) return MG_ERR_ALLOC;
    for (uint32_t i = 0; i < n; i++) {
        nodes[i].index    = -1;
        nodes[i].lowlink  = -1;
        nodes[i].on_stack = false;
    }

    uint32_t *scc_stack = (uint32_t *)mg_pool_alloc(
        pool, sizeof(uint32_t) * n);
    if (!scc_stack) return MG_ERR_ALLOC;

    tarjan_frame_t *call_stack = (tarjan_frame_t *)mg_pool_alloc(
        pool, sizeof(tarjan_frame_t) * n);
    if (!call_stack) return MG_ERR_ALLOC;

    mg_retain_cycle_t *cycles = (mg_retain_cycle_t *)mg_pool_alloc(
        pool, sizeof(mg_retain_cycle_t) * MG_MAX_CYCLES_REPORT);
    if (!cycles) return MG_ERR_ALLOC;

    /* Bitsets for path extraction. */
    uint32_t bitset_bytes = (n + 7) / 8;
    uint8_t *scc_member = (uint8_t *)mg_pool_alloc(pool, bitset_bytes);
    uint8_t *visited    = (uint8_t *)mg_pool_alloc(pool, bitset_bytes);
    if (!scc_member || !visited) return MG_ERR_ALLOC;

    bool filter_system = opts ? opts->filter_system_cycles : true;

    tarjan_ctx_t ctx = {
        .g              = g,
        .nodes          = nodes,
        .scc_stack      = scc_stack,
        .scc_top        = 0,
        .index_counter  = 0,
        .call_stack     = call_stack,
        .call_stack_cap = n,
        .pool           = pool,
        .ivar_map       = ivar_map,
        .filter_system  = filter_system,
        .cycles         = cycles,
        .cycle_count    = 0,
        .max_cycles     = MG_MAX_CYCLES_REPORT,
        .scc_member     = scc_member,
        .visited        = visited,
        .bitset_bytes   = bitset_bytes,
        .cycles_dropped_length  = 0,
        .cycles_dropped_limit   = 0,
        .system_cycles_filtered = 0,
    };

    tarjan_run(&ctx);

    /* Sort cycles by impact (total_size descending). */
    if (ctx.cycle_count > 1) {
        qsort(ctx.cycles, ctx.cycle_count,
              sizeof(mg_retain_cycle_t), cmp_cycle_impact);
    }

    out->cycles      = cycles;
    out->cycle_count = ctx.cycle_count;
    out->cycles_dropped_length  = ctx.cycles_dropped_length;
    out->cycles_dropped_limit   = ctx.cycles_dropped_limit;
    out->system_cycles_filtered = ctx.system_cycles_filtered;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read-only accessors                                                 */
/* ------------------------------------------------------------------ */

int32_t mg_ref_graph_lookup(const mg_ref_graph_t *g, uint64_t address) {
    if (!g || !g->addr_map) return -1;
    uint32_t idx = addr_lookup(g, address);
    return (idx == UINT32_MAX) ? -1 : (int32_t)idx;
}

const mg_ref_obj_t *mg_ref_graph_objects(const mg_ref_graph_t *g,
                                          uint32_t *count) {
    if (!g) { if (count) *count = 0; return NULL; }
    if (count) *count = g->obj_count;
    return g->objects;
}

const uint32_t *mg_ref_graph_adj_offsets(const mg_ref_graph_t *g) {
    return g ? g->adj_offsets : NULL;
}

const uint32_t *mg_ref_graph_adj_targets(const mg_ref_graph_t *g) {
    return g ? g->adj_targets : NULL;
}
