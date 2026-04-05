/*
 * mg_reach.c — Reachability-based leak detection
 *
 * Phase C (threads frozen):
 *   Scans GC roots (thread stacks, registers, DATA segments) for
 *   pointers to known objects in the reference graph.
 *
 * Phase E (threads running):
 *   BFS from root objects, marks reachable set, groups unreachable
 *   objects by class.
 */

#include "mg_reach.h"
#include "mg_ref_graph.h"
#include "mg_vm_region.h"
#include "mg_thread.h"
#include "mg_pool.h"
#include "../include/memory_graph.h"   /* MG_ERR_* codes */

#include <string.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

/* ------------------------------------------------------------------ */
/* Bitset helpers                                                      */
/* ------------------------------------------------------------------ */

#define BIT_SET(bits, i)   ((bits)[(i) >> 3] |=  (uint8_t)(1u << ((i) & 7)))
#define BIT_TEST(bits, i)  (((bits)[(i) >> 3] &  (uint8_t)(1u << ((i) & 7))) != 0)

/* ------------------------------------------------------------------ */
/* VM tag classification                                               */
/* ------------------------------------------------------------------ */

static bool is_stack_tag(uint32_t tag) {
    return tag == VM_MEMORY_STACK;
}

static bool is_malloc_tag(uint32_t tag) {
    switch (tag) {
    case VM_MEMORY_MALLOC:
    case VM_MEMORY_MALLOC_SMALL:
    case VM_MEMORY_MALLOC_LARGE:
    case VM_MEMORY_MALLOC_HUGE:
    case VM_MEMORY_MALLOC_TINY:
    case VM_MEMORY_MALLOC_LARGE_REUSABLE:
    case VM_MEMORY_MALLOC_LARGE_REUSED:
#ifdef VM_MEMORY_MALLOC_NANO
    case VM_MEMORY_MALLOC_NANO:
#endif
        return true;
    default:
        return false;
    }
}

/* A DATA-like region: writable, not stack, not heap. */
static bool is_data_region(const mg_vm_region_t *r) {
    if (!(r->protection & VM_PROT_WRITE)) return false;
    if (is_stack_tag(r->user_tag))        return false;
    if (is_malloc_tag(r->user_tag))       return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Root pointer scanner                                                */
/* ------------------------------------------------------------------ */

/*
 * Scan a memory range for 8-byte-aligned pointers to known objects.
 * Marks matching objects in `root_bits` and increments `*root_count`.
 */
static void scan_region_for_roots(const mg_ref_graph_t *graph,
                                  uint64_t base, uint64_t size,
                                  uint8_t *root_bits,
                                  uint32_t *root_count) {
    if (size < 8) return;

    const uint64_t *start = (const uint64_t *)base;
    const uint64_t *end   = (const uint64_t *)(base + (size & ~7ULL));

    for (const uint64_t *p = start; p < end; p++) {
        uint64_t val = *p;

        /* Quick filter: null, tagged pointers, unaligned. */
        if (val == 0) continue;
        if (val & (1ULL << 63)) continue;   /* tagged pointer */
        if (val & 0x7) continue;            /* unaligned */

        int32_t idx = mg_ref_graph_lookup(graph, val);
        if (idx >= 0 && !BIT_TEST(root_bits, idx)) {
            BIT_SET(root_bits, idx);
            (*root_count)++;
        }
    }
}

/*
 * Check a single 64-bit value (e.g. from a register) as a potential root.
 */
static void check_root_value(const mg_ref_graph_t *graph,
                             uint64_t val,
                             uint8_t *root_bits,
                             uint32_t *root_count) {
    if (val == 0) return;
    if (val & (1ULL << 63)) return;
    if (val & 0x7) return;

    int32_t idx = mg_ref_graph_lookup(graph, val);
    if (idx >= 0 && !BIT_TEST(root_bits, idx)) {
        BIT_SET(root_bits, idx);
        (*root_count)++;
    }
}

/* ------------------------------------------------------------------ */
/* Phase C: Scan GC roots                                              */
/* ------------------------------------------------------------------ */

int mg_reach_scan_roots(mg_pool_t *pool,
                        const mg_ref_graph_t *graph,
                        const mg_vm_snapshot_t *vm_snap,
                        const mg_thread_state_t *threads,
                        uint8_t **root_bits_out,
                        uint32_t *root_count_out) {
    uint32_t obj_count = 0;
    mg_ref_graph_objects(graph, &obj_count);
    if (obj_count == 0) {
        *root_bits_out  = NULL;
        *root_count_out = 0;
        return 0;
    }

    /* Allocate bitset: one bit per object. */
    uint32_t bitset_bytes = (obj_count + 7) / 8;
    uint8_t *bits = (uint8_t *)mg_pool_alloc(pool, bitset_bytes);
    if (!bits) return MG_ERR_ALLOC;
    memset(bits, 0, bitset_bytes);

    uint32_t root_count = 0;

    /* --- 1. Scan thread stacks --- */
    if (vm_snap) {
        for (uint32_t i = 0; i < vm_snap->region_count; i++) {
            const mg_vm_region_t *r = &vm_snap->regions[i];
            if (!is_stack_tag(r->user_tag)) continue;
            if (!(r->protection & VM_PROT_READ)) continue;
            if (r->resident_size == 0) continue;

            scan_region_for_roots(graph, r->address, r->virtual_size,
                                  bits, &root_count);
        }
    }

    /* --- 2. Scan suspended thread registers --- */
#ifndef MG_APP_STORE_COMPLIANT
    if (threads && threads->count > 0) {
        for (mach_msg_type_number_t t = 0; t < threads->count; t++) {
#if defined(__arm64__)
            arm_thread_state64_t state;
            mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
            kern_return_t kr = thread_get_state(
                threads->threads[t], ARM_THREAD_STATE64,
                (thread_state_t)&state, &state_count);
            if (kr != KERN_SUCCESS) continue;

            /* Scan general-purpose registers x0-x28, fp, lr, sp. */
            for (int r = 0; r < 29; r++) {
                check_root_value(graph, state.__x[r], bits, &root_count);
            }
            check_root_value(graph, (uint64_t)__darwin_arm_thread_state64_get_fp(state),
                             bits, &root_count);
            check_root_value(graph, (uint64_t)__darwin_arm_thread_state64_get_lr(state),
                             bits, &root_count);
            check_root_value(graph, (uint64_t)__darwin_arm_thread_state64_get_sp(state),
                             bits, &root_count);
#elif defined(__x86_64__)
            x86_thread_state64_t state;
            mach_msg_type_number_t state_count = x86_THREAD_STATE64_COUNT;
            kern_return_t kr = thread_get_state(
                threads->threads[t], x86_THREAD_STATE64,
                (thread_state_t)&state, &state_count);
            if (kr != KERN_SUCCESS) continue;

            /* Scan GP registers. */
            check_root_value(graph, state.__rax, bits, &root_count);
            check_root_value(graph, state.__rbx, bits, &root_count);
            check_root_value(graph, state.__rcx, bits, &root_count);
            check_root_value(graph, state.__rdx, bits, &root_count);
            check_root_value(graph, state.__rdi, bits, &root_count);
            check_root_value(graph, state.__rsi, bits, &root_count);
            check_root_value(graph, state.__rbp, bits, &root_count);
            check_root_value(graph, state.__rsp, bits, &root_count);
            check_root_value(graph, state.__r8,  bits, &root_count);
            check_root_value(graph, state.__r9,  bits, &root_count);
            check_root_value(graph, state.__r10, bits, &root_count);
            check_root_value(graph, state.__r11, bits, &root_count);
            check_root_value(graph, state.__r12, bits, &root_count);
            check_root_value(graph, state.__r13, bits, &root_count);
            check_root_value(graph, state.__r14, bits, &root_count);
            check_root_value(graph, state.__r15, bits, &root_count);
#endif
        }
    }
#endif /* MG_APP_STORE_COMPLIANT */

    /* --- 3. Scan writable DATA-like regions --- */
    if (vm_snap) {
        for (uint32_t i = 0; i < vm_snap->region_count; i++) {
            const mg_vm_region_t *r = &vm_snap->regions[i];
            if (!is_data_region(r)) continue;
            if (!(r->protection & VM_PROT_READ)) continue;
            if (r->resident_size == 0) continue;

            /* Skip very large regions (> 4 MB) to bound Phase C time. */
            if (r->virtual_size > 4 * 1024 * 1024) continue;

            scan_region_for_roots(graph, r->address, r->virtual_size,
                                  bits, &root_count);
        }
    }

    *root_bits_out  = bits;
    *root_count_out = root_count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase E: BFS reachability + aggregation                             */
/* ------------------------------------------------------------------ */

/* Class hash table for aggregating unreachable objects. */
#define REACH_HT_CAP  4096
#define REACH_HT_MASK (REACH_HT_CAP - 1)

typedef struct {
    const char *class_name;   /* NULL = empty slot */
    uint32_t    count;
    uint64_t    total_size;
} reach_ht_entry_t;

static uint32_t fnv1a_ptr(const void *ptr) {
    uint64_t v = (uint64_t)ptr;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 8; i++) {
        h ^= (uint8_t)(v >> (i * 8));
        h *= 0x01000193u;
    }
    return h;
}

int mg_reach_analyze(mg_pool_t *pool,
                     const mg_ref_graph_t *graph,
                     const uint8_t *root_bits,
                     mg_reach_result_t *out) {
    memset(out, 0, sizeof(*out));
    if (!root_bits) return 0;

    uint32_t obj_count = 0;
    const mg_ref_obj_t *objects = mg_ref_graph_objects(graph, &obj_count);
    if (obj_count == 0) return 0;

    const uint32_t *adj_off = mg_ref_graph_adj_offsets(graph);
    const uint32_t *adj_tgt = mg_ref_graph_adj_targets(graph);
    if (!adj_off || !adj_tgt) return 0;

    /* Allocate visited bitset. */
    uint32_t bitset_bytes = (obj_count + 7) / 8;
    uint8_t *visited = (uint8_t *)mg_pool_alloc(pool, bitset_bytes);
    if (!visited) return MG_ERR_ALLOC;
    memset(visited, 0, bitset_bytes);

    /* BFS queue (indices). */
    uint32_t *queue = (uint32_t *)mg_pool_alloc(pool, sizeof(uint32_t) * obj_count);
    if (!queue) return MG_ERR_ALLOC;

    /* Seed queue with root objects. */
    uint32_t head = 0, tail = 0;
    for (uint32_t i = 0; i < obj_count; i++) {
        if (BIT_TEST(root_bits, i)) {
            BIT_SET(visited, i);
            queue[tail++] = i;
            out->root_count++;
        }
    }

    /* BFS traversal. */
    while (head < tail) {
        uint32_t cur = queue[head++];

        uint32_t edge_begin = adj_off[cur];
        uint32_t edge_end   = adj_off[cur + 1];

        for (uint32_t e = edge_begin; e < edge_end; e++) {
            uint32_t target = MG_EDGE_TARGET(adj_tgt[e]);
            if (target >= obj_count) continue;
            if (!BIT_TEST(visited, target)) {
                BIT_SET(visited, target);
                queue[tail++] = target;
            }
        }
    }

    out->total_reachable = tail;

    /* Count unreachable objects and aggregate by class. */
    reach_ht_entry_t *ht = (reach_ht_entry_t *)mg_pool_alloc(
        pool, sizeof(reach_ht_entry_t) * REACH_HT_CAP);
    if (!ht) return MG_ERR_ALLOC;
    memset(ht, 0, sizeof(reach_ht_entry_t) * REACH_HT_CAP);

    uint32_t total_unreachable = 0;
    uint32_t unique_classes    = 0;

    for (uint32_t i = 0; i < obj_count; i++) {
        if (BIT_TEST(visited, i)) continue;

        total_unreachable++;
        const char *name = objects[i].class_name;
        uint32_t slot = fnv1a_ptr(name) & REACH_HT_MASK;

        /* Open-address lookup by class_name pointer (interned by runtime). */
        while (ht[slot].class_name != NULL && ht[slot].class_name != name) {
            slot = (slot + 1) & REACH_HT_MASK;
        }

        if (ht[slot].class_name == NULL) {
            ht[slot].class_name = name;
            unique_classes++;
        }
        ht[slot].count++;
        ht[slot].total_size += objects[i].size;
    }

    out->total_unreachable = total_unreachable;

    if (unique_classes == 0) return 0;

    /* Build sorted suspect array (by total_size desc). */
    mg_reach_suspect_t *suspects = (mg_reach_suspect_t *)mg_pool_alloc(
        pool, sizeof(mg_reach_suspect_t) * unique_classes);
    if (!suspects) return MG_ERR_ALLOC;

    uint32_t idx = 0;
    for (uint32_t i = 0; i < REACH_HT_CAP; i++) {
        if (ht[i].class_name == NULL) continue;
        suspects[idx].class_name = ht[i].class_name;
        suspects[idx].count      = ht[i].count;
        suspects[idx].total_size = ht[i].total_size;
        idx++;
    }

    /* Simple insertion sort (small N). */
    for (uint32_t i = 1; i < unique_classes; i++) {
        mg_reach_suspect_t tmp = suspects[i];
        uint32_t j = i;
        while (j > 0 && suspects[j - 1].total_size < tmp.total_size) {
            suspects[j] = suspects[j - 1];
            j--;
        }
        suspects[j] = tmp;
    }

    out->suspects      = suspects;
    out->suspect_count = unique_classes;
    return 0;
}
