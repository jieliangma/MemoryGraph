/*
 * mg_heap.c — Heap object enumeration and ObjC identification
 *
 * 1. Builds a sorted set of all registered ObjC class pointers
 *    (before thread suspension).
 * 2. Enumerates every in-use block across all malloc zones.
 * 3. For each block, reads the isa pointer and validates it
 *    against the class set to identify ObjC/Swift objects.
 * 4. Aggregates statistics per class in a hash table.
 * 5. Extracts the Top-N classes by total size.
 *
 * No malloc/free calls during enumeration — all scratch memory
 * comes from the pre-allocated pool.
 */

#include "mg_heap.h"
#include "mg_ref_graph.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <objc/runtime.h>
#include <string.h>
#include <stdlib.h>  /* qsort */

/* ================================================================== */
/* Class set — sorted array for binary-search validation               */
/* ================================================================== */

struct mg_class_set {
    Class *classes;
    int    count;
};

static int cmp_class_ptr(const void *a, const void *b) {
    uintptr_t pa = (uintptr_t)*(const Class *)a;
    uintptr_t pb = (uintptr_t)*(const Class *)b;
    if (pa < pb) return -1;
    if (pa > pb) return  1;
    return 0;
}

mg_class_set_t *mg_class_set_build(mg_pool_t *pool) {
    if (!pool) return NULL;

    /* Allocate the set header. */
    mg_class_set_t *set = (mg_class_set_t *)mg_pool_alloc(
        pool, sizeof(mg_class_set_t));
    if (!set) return NULL;

    /* Query number of registered classes. */
    int count = objc_getClassList(NULL, 0);
    if (count <= 0) {
        set->classes = NULL;
        set->count   = 0;
        return set;
    }

    /* Allocate buffer with some slack for classes registered between
     * the count query and the fill call. */
#define MG_CLASS_SET_SLACK 256
    int capacity = count + MG_CLASS_SET_SLACK;
    set->classes = (Class *)mg_pool_alloc(pool, sizeof(Class) * (size_t)capacity);
    if (!set->classes) {
        set->count = 0;
        return set;
    }

    set->count = objc_getClassList(set->classes, capacity);

    /* Sort for binary search. */
    qsort(set->classes, (size_t)set->count, sizeof(Class), cmp_class_ptr);
    return set;
}

static bool class_set_contains(const mg_class_set_t *set, Class cls) {
    if (!set || set->count == 0) return false;

    int lo = 0, hi = set->count - 1;
    uintptr_t target = (uintptr_t)cls;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uintptr_t val = (uintptr_t)set->classes[mid];
        if (val == target) return true;
        if (val < target)  lo = mid + 1;
        else               hi = mid - 1;
    }
    return false;
}

/* ================================================================== */
/* ISA pointer extraction                                              */
/* ================================================================== */

/* ISA pointer masks for tagged isa extraction.
 * arm64 (non-ptrauth): bits [3:32] contain the class pointer.
 * arm64e (ptrauth):    bits [3:50] contain the class pointer.
 * See objc-runtime-new.h in Apple's open source for authoritative values. */
#define MG_ISA_MASK_ARM64   0x0000000ffffffff8ULL
#define MG_ISA_MASK_ARM64E  0x007ffffffffffff8ULL

/*
 * Extract the Class pointer from a raw isa value.
 * Returns NULL if it doesn't match any known class.
 *
 * arm64 uses tagged isa with ISA_MASK.
 * arm64e (ptrauth) has a different mask.
 * x86_64 has no mask (direct pointer).
 */
/*
 * Compile-time ISA mask selection.
 * __has_feature(ptrauth_calls) is the canonical Apple Clang check
 * for arm64e (pointer authentication).  Using it at compile time
 * eliminates a redundant binary search per heap object.
 */
#if defined(__arm64__)
  #if __has_feature(ptrauth_calls)
    #define MG_ISA_MASK MG_ISA_MASK_ARM64E
  #else
    #define MG_ISA_MASK MG_ISA_MASK_ARM64
  #endif
#endif

static Class extract_class(uintptr_t isa_raw, const mg_class_set_t *set) {
#if defined(__arm64__)
    uintptr_t cls = isa_raw & MG_ISA_MASK;
    if (class_set_contains(set, (Class)cls)) return (Class)cls;
#elif defined(__x86_64__)
    if (class_set_contains(set, (Class)isa_raw)) return (Class)isa_raw;
#endif
    return NULL;
}

/* ================================================================== */
/* Class stats hash table (open addressing, pool-allocated)            */
/* ================================================================== */

#define CLASS_TABLE_CAPACITY   16384   /* must be power of 2 */
#define CLASS_TABLE_MASK       (CLASS_TABLE_CAPACITY - 1)
#define CLASS_TABLE_LOAD_LIMIT (CLASS_TABLE_CAPACITY * 3 / 4)

typedef struct {
    const char *name;    /* NULL = empty slot */
    uint32_t    count;
    uint64_t    total_size;
} class_entry_t;

typedef struct {
    class_entry_t *entries;
    uint32_t       count;
} class_table_t;

static class_table_t *class_table_create(mg_pool_t *pool) {
    class_table_t *t = (class_table_t *)mg_pool_alloc(
        pool, sizeof(class_table_t));
    if (!t) return NULL;

    t->entries = (class_entry_t *)mg_pool_alloc(
        pool, sizeof(class_entry_t) * CLASS_TABLE_CAPACITY);
    if (!t->entries) return NULL;

    memset(t->entries, 0, sizeof(class_entry_t) * CLASS_TABLE_CAPACITY);
    t->count = 0;
    return t;
}

static inline uint32_t hash_ptr(uintptr_t p) {
    p ^= p >> 33;
    p *= 0xff51afd7ed558ccdULL;
    p ^= p >> 33;
    return (uint32_t)p;
}

static void class_table_insert(class_table_t *t,
                               const char *name,
                               uint64_t size) {
    if (!t || !name) return;
    if (t->count >= CLASS_TABLE_LOAD_LIMIT) return; /* 75% load factor */

    uint32_t idx = hash_ptr((uintptr_t)name) & CLASS_TABLE_MASK;

    while (1) {
        if (t->entries[idx].name == NULL) {
            /* Empty slot — insert. */
            t->entries[idx].name       = name;
            t->entries[idx].count      = 1;
            t->entries[idx].total_size = size;
            t->count++;
            return;
        }
        if (t->entries[idx].name == name) {
            /* Same class (pointer equality, since class_getName
             * returns stable pointers for the same Class). */
            t->entries[idx].count++;
            t->entries[idx].total_size += size;
            return;
        }
        idx = (idx + 1) & CLASS_TABLE_MASK;
    }
}

/* ================================================================== */
/* Buffer table — non-ObjC malloc blocks for collection-indirect scan  */
/* ================================================================== */

/* Max buffer size to record (skip large data blobs like images). */
#define MG_BUFFER_MAX_SIZE   (256u * 1024)
/* Min buffer size (must hold at least one pointer). */
#define MG_BUFFER_MIN_SIZE   (sizeof(void *) * 2)
/* Max entries in the buffer table. */
#define MG_BUFFER_TABLE_CAP  100000

static mg_buffer_table_t *buffer_table_create(mg_pool_t *pool) {
    mg_buffer_table_t *bt = (mg_buffer_table_t *)mg_pool_alloc(
        pool, sizeof(mg_buffer_table_t));
    if (!bt) return NULL;

    bt->capacity = MG_BUFFER_TABLE_CAP;
    bt->entries = (mg_buffer_entry_t *)mg_pool_alloc(
        pool, sizeof(mg_buffer_entry_t) * bt->capacity);
    if (!bt->entries) { bt->capacity = 0; return bt; }
    bt->count = 0;
    return bt;
}

static void buffer_table_add(mg_buffer_table_t *bt,
                             uint64_t addr, uint32_t size) {
    if (!bt || bt->count >= bt->capacity) return;
    if (size < MG_BUFFER_MIN_SIZE || size > MG_BUFFER_MAX_SIZE) return;
    bt->entries[bt->count].addr = addr;
    bt->entries[bt->count].size = size;
    bt->count++;
}

static int cmp_buffer_entry(const void *a, const void *b) {
    uint64_t aa = ((const mg_buffer_entry_t *)a)->addr;
    uint64_t bb = ((const mg_buffer_entry_t *)b)->addr;
    if (aa < bb) return -1;
    if (aa > bb) return  1;
    return 0;
}

static void buffer_table_sort(mg_buffer_table_t *bt) {
    if (!bt || bt->count <= 1) return;
    qsort(bt->entries, bt->count, sizeof(mg_buffer_entry_t), cmp_buffer_entry);
}

const mg_buffer_entry_t *mg_buffer_table_lookup(const mg_buffer_table_t *bt,
                                                 uint64_t addr) {
    if (!bt || bt->count == 0) return NULL;

    uint32_t lo = 0, hi = bt->count - 1;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t start = bt->entries[mid].addr;
        uint64_t end   = start + bt->entries[mid].size;
        if (addr >= start && addr < end) return &bt->entries[mid];
        if (addr < start) {
            if (mid == 0) break;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

/* ================================================================== */
/* Malloc zone enumeration                                             */
/* ================================================================== */

/* Map allocation size to bucket index [0..5]. */
static inline uint32_t raw_size_bucket(uint32_t size) {
    if (size <    256) return 0;  /* [16, 256)   */
    if (size <   1024) return 1;  /* [256, 1K)   */
    if (size <   4096) return 2;  /* [1K, 4K)    */
    if (size <  16384) return 3;  /* [4K, 16K)   */
    if (size < 262144) return 4;  /* [16K, 256K) */
    return 5;                     /* [256K+)     */
}

typedef struct {
    const mg_class_set_t *class_set;
    class_table_t        *class_table;
    mg_ref_graph_t       *ref_graph;     /* nullable */
    mg_buffer_table_t    *buffer_table;  /* nullable */
    uint64_t              total_blocks;
    uint64_t              total_size;
    uint64_t              total_objects;
    /* Raw (non-ObjC) allocation stats. */
    uint64_t              raw_total_size;
    uint32_t              raw_buckets[MG_RAW_BUCKET_COUNT];
} enum_ctx_t;

/*
 * Callback invoked by the malloc zone enumerator for each batch
 * of in-use memory ranges.
 */
static void range_recorder(task_t task, void *ctx,
                           unsigned type,
                           vm_range_t *ranges, unsigned count) {
    (void)task;
    (void)type;
    enum_ctx_t *c = (enum_ctx_t *)ctx;

    for (unsigned i = 0; i < count; i++) {
        vm_address_t addr = ranges[i].address;
        vm_size_t    size = ranges[i].size;

        c->total_blocks++;
        c->total_size += size;

        /* Need at least a pointer-sized allocation for isa. */
        if (size < sizeof(void *)) continue;

        /* Prefetch the next range's ISA pointer to hide cache miss
         * latency (~100+ cycles for random heap addresses).
         * PLDL1STRM: load into L1 with streaming hint (won't pollute
         * cache if not accessed again). */
        if (i + 1 < count && ranges[i + 1].size >= sizeof(void *)) {
            __builtin_prefetch((void *)ranges[i + 1].address, 0, 0);
        }

        /* Read potential isa pointer. */
        uintptr_t isa_raw = *(uintptr_t *)addr;
        if (isa_raw == 0) {
            /* Not an ObjC object — record as C/C++ allocation. */
            c->raw_total_size += size;
            c->raw_buckets[raw_size_bucket((uint32_t)size)]++;
            if (c->buffer_table)
                buffer_table_add(c->buffer_table, (uint64_t)addr, (uint32_t)size);
            continue;
        }

        Class cls = extract_class(isa_raw, c->class_set);
        if (!cls) {
            /* Not an ObjC object — record as C/C++ allocation. */
            c->raw_total_size += size;
            c->raw_buckets[raw_size_bucket((uint32_t)size)]++;
            if (c->buffer_table)
                buffer_table_add(c->buffer_table, (uint64_t)addr, (uint32_t)size);
            continue;
        }

        c->total_objects++;
        const char *name = class_getName(cls);
        class_table_insert(c->class_table, name, size);

        if (c->ref_graph) {
            /* Detect ObjC block heap objects. */
            uint8_t flags = 0;
            if (name[0] == '_' && name[1] == '_' &&
                strncmp(name + 2, "NS", 2) == 0 &&
                strstr(name + 4, "Block") != NULL) {
                flags |= MG_OBJ_FLAG_BLOCK;
            }
            mg_ref_graph_add_object(c->ref_graph, (uint64_t)addr,
                                    (uint32_t)size, name, flags);
        }
    }
}

/*
 * Simple in-process memory reader for malloc zone enumeration.
 */
static kern_return_t local_reader(task_t task,
                                  vm_address_t address,
                                  vm_size_t size,
                                  void **out) {
    (void)task;
    (void)size;
    *out = (void *)address;
    return KERN_SUCCESS;
}

/* ================================================================== */
/* Top-N extraction                                                    */
/* ================================================================== */

/* Min-heap helpers for top-N selection.
 * O(K log N) instead of O(K log K) full sort. */
static void heap_sift_down(mg_class_stat_t *h, uint32_t size, uint32_t i) {
    while (1) {
        uint32_t smallest = i;
        uint32_t l = 2 * i + 1;
        uint32_t r = 2 * i + 2;
        if (l < size && h[l].total_size < h[smallest].total_size)
            smallest = l;
        if (r < size && h[r].total_size < h[smallest].total_size)
            smallest = r;
        if (smallest == i) break;
        mg_class_stat_t tmp = h[i];
        h[i] = h[smallest];
        h[smallest] = tmp;
        i = smallest;
    }
}

static uint32_t extract_top_n(class_table_t *table,
                              mg_pool_t *pool,
                              uint32_t n,
                              mg_class_stat_t **out) {
    if (!table || table->count == 0 || n == 0) {
        *out = NULL;
        return 0;
    }

    /* Allocate only N entries instead of all K. */
    uint32_t heap_cap = table->count < n ? table->count : n;
    mg_class_stat_t *heap = (mg_class_stat_t *)mg_pool_alloc(
        pool, sizeof(mg_class_stat_t) * heap_cap);
    if (!heap) {
        *out = NULL;
        return 0;
    }

    uint32_t heap_size = 0;
    for (uint32_t i = 0; i < CLASS_TABLE_CAPACITY; i++) {
        if (table->entries[i].name == NULL) continue;

        mg_class_stat_t entry;
        entry.class_name = table->entries[i].name;
        entry.count      = table->entries[i].count;
        entry.total_size = table->entries[i].total_size;

        if (heap_size < heap_cap) {
            /* Heap not full: insert and sift up. */
            heap[heap_size] = entry;
            uint32_t k = heap_size;
            while (k > 0) {
                uint32_t parent = (k - 1) / 2;
                if (heap[parent].total_size <= heap[k].total_size) break;
                mg_class_stat_t tmp = heap[parent];
                heap[parent] = heap[k];
                heap[k] = tmp;
                k = parent;
            }
            heap_size++;
        } else if (entry.total_size > heap[0].total_size) {
            /* Entry is larger than min in heap: replace root and sift down. */
            heap[0] = entry;
            heap_sift_down(heap, heap_size, 0);
        }
    }

    /* Heapsort to get descending order. */
    for (uint32_t i = heap_size; i > 1; i--) {
        mg_class_stat_t tmp = heap[0];
        heap[0] = heap[i - 1];
        heap[i - 1] = tmp;
        heap_sift_down(heap, i - 1, 0);
    }

    *out = heap;
    return heap_size;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int mg_heap_collect(mg_pool_t *pool,
                    const mg_class_set_t *class_set,
                    uint32_t top_n,
                    mg_heap_snapshot_t *out,
                    mg_ref_graph_t *ref_graph) {
    if (!pool || !out) return -1;
    memset(out, 0, sizeof(*out));

    class_table_t *table = class_table_create(pool);
    if (!table) return -1;

    /* Create buffer table for indirect reference scanning. */
    mg_buffer_table_t *bt = ref_graph ? buffer_table_create(pool) : NULL;

    /* Get all malloc zones. */
    vm_address_t *zone_addrs = NULL;
    unsigned zone_count = 0;
    kern_return_t kr = malloc_get_all_zones(
        mach_task_self(), local_reader, &zone_addrs, &zone_count);
    if (kr != KERN_SUCCESS || zone_count == 0) return -1;

    if (zone_count > MG_MAX_ZONES) zone_count = MG_MAX_ZONES;

    /* Enumerate each zone. */
    for (unsigned z = 0; z < zone_count; z++) {
        malloc_zone_t *zone = (malloc_zone_t *)zone_addrs[z];

        /* Some zones may not have introspection. */
        if (!zone->introspect || !zone->introspect->enumerator) {
            continue;
        }

        enum_ctx_t ctx = {
            .class_set    = class_set,
            .class_table  = table,
            .ref_graph    = ref_graph,
            .buffer_table = bt,
            .total_blocks = 0,
            .total_size   = 0,
            .total_objects = 0,
        };

        zone->introspect->enumerator(
            mach_task_self(),
            &ctx,
            MALLOC_PTR_IN_USE_RANGE_TYPE,
            (vm_address_t)zone,
            local_reader,
            range_recorder);

        /* Record per-zone stats. */
        out->zones[z].name      = malloc_get_zone_name(zone);
        out->zones[z].allocated = ctx.total_size;
        out->zones[z].blocks    = ctx.total_blocks;

        out->total_allocated += ctx.total_size;
        out->total_blocks    += ctx.total_blocks;
        out->total_objects   += ctx.total_objects;

        /* Accumulate raw (C/C++) allocation stats. */
        out->raw_stat.raw_total_size += ctx.raw_total_size;
        for (int b = 0; b < MG_RAW_BUCKET_COUNT; b++)
            out->raw_stat.raw_size_buckets[b] += ctx.raw_buckets[b];
    }
    out->zone_count       = zone_count;
    out->raw_allocations  = out->total_blocks - out->total_objects;

    /* Sort buffer table for binary search. */
    if (bt) buffer_table_sort(bt);
    out->buffer_table = bt;

    /* Extract Top-N classes. */
    out->top_count = extract_top_n(
        table, pool, top_n, &out->top_classes);

    return 0;
}
