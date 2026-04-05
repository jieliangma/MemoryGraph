/*
 * mg_ivar_map.c — ObjC ivar offset-to-name lookup table
 *
 * Builds a hash table of (class_name_ptr, byte_offset) -> ivar_name
 * by introspecting all loaded ObjC classes via class_copyIvarList.
 *
 * Only object-typed ivars (encoding starting with '@') are recorded.
 * Lookup walks the superclass chain for inherited ivars.
 */

#include "mg_ivar_map.h"

#ifdef MG_APP_STORE_COMPLIANT

/* No ivar map in App Store compliant mode (no ObjC runtime introspection). */

mg_ivar_map_t *mg_ivar_map_build(mg_pool_t *pool) {
    (void)pool;
    return NULL;
}

const char *mg_ivar_map_lookup(const mg_ivar_map_t *map,
                               const char *class_name,
                               uint16_t byte_offset) {
    (void)map; (void)class_name; (void)byte_offset;
    return NULL;
}

#else /* full implementation */

#include <objc/runtime.h>
#include <string.h>
#include <stdlib.h>   /* free — for class_copyIvarList result */

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *class_name;  /* interned via class_getName — stable */
    uint16_t    offset;
    const char *ivar_name;   /* interned via ivar_getName — stable */
} ivar_entry_t;

/* Hash table: open addressing, linear probing. */
typedef struct {
    uint32_t entry_idx;  /* 1-based index into entries[] (0 = empty) */
} ivar_slot_t;

struct mg_ivar_map {
    ivar_entry_t *entries;
    uint32_t      entry_count;
    uint32_t      entry_capacity;

    ivar_slot_t  *slots;
    uint32_t      slot_mask;  /* capacity - 1 */
};

/* ------------------------------------------------------------------ */
/* Hash                                                                */
/* ------------------------------------------------------------------ */

static inline uint32_t hash_ivar(const char *cls_name, uint16_t offset) {
    /* Hash the class name pointer (interned, unique per class) + offset. */
    uint64_t v = (uint64_t)(uintptr_t)cls_name;
    v ^= (uint64_t)offset * 0x9e3779b97f4a7c15ULL;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

mg_ivar_map_t *mg_ivar_map_build(mg_pool_t *pool) {
    if (!pool) return NULL;

    /* Count classes. */
    int class_count = objc_getClassList(NULL, 0);
    if (class_count <= 0) return NULL;

    /* Allocate temp class list (from pool — it stays allocated). */
    uint32_t capacity = (uint32_t)class_count + 256;
    Class *classes = (Class *)mg_pool_alloc(pool, sizeof(Class) * capacity);
    if (!classes) return NULL;

    class_count = objc_getClassList(classes, (int)capacity);
    if (class_count <= 0) return NULL;

    /* --- Pass 1: count object-typed ivars for exact allocation --- */
    uint32_t total_ivars = 0;
    for (int c = 0; c < class_count; c++) {
        unsigned ivar_count = 0;
        Ivar *ivars = class_copyIvarList(classes[c], &ivar_count);
        if (!ivars) continue;

        for (unsigned i = 0; i < ivar_count; i++) {
            const char *enc = ivar_getTypeEncoding(ivars[i]);
            if (enc && enc[0] != '@') continue;
            ptrdiff_t off = ivar_getOffset(ivars[i]);
            if (off < 0 || off > 0xFFFF) continue;
            total_ivars++;
        }
        free(ivars);
    }
    if (total_ivars == 0) return NULL;

    /* Allocate map. */
    mg_ivar_map_t *map = (mg_ivar_map_t *)mg_pool_alloc(
        pool, sizeof(mg_ivar_map_t));
    if (!map) return NULL;
    memset(map, 0, sizeof(*map));

    map->entry_capacity = total_ivars;
    map->entries = (ivar_entry_t *)mg_pool_alloc(
        pool, sizeof(ivar_entry_t) * total_ivars);
    if (!map->entries) return NULL;

    /* Hash table: 2x entries, power of 2. */
    uint32_t slot_cap = 1;
    while (slot_cap < total_ivars * 2) slot_cap <<= 1;
    map->slot_mask = slot_cap - 1;
    map->slots = (ivar_slot_t *)mg_pool_alloc(
        pool, sizeof(ivar_slot_t) * slot_cap);
    if (!map->slots) return NULL;
    memset(map->slots, 0, sizeof(ivar_slot_t) * slot_cap);

    /* --- Pass 2: populate entries and hash table --- */
    for (int c = 0; c < class_count; c++) {
        unsigned ivar_count = 0;
        Ivar *ivars = class_copyIvarList(classes[c], &ivar_count);
        if (!ivars) continue;

        const char *cls_name = class_getName(classes[c]);

        for (unsigned i = 0; i < ivar_count; i++) {
            const char *enc = ivar_getTypeEncoding(ivars[i]);
            if (enc && enc[0] != '@') continue;

            ptrdiff_t off = ivar_getOffset(ivars[i]);
            if (off < 0 || off > 0xFFFF) continue;

            if (map->entry_count >= map->entry_capacity) break;

            uint32_t idx = map->entry_count;
            map->entries[idx].class_name = cls_name;
            map->entries[idx].offset     = (uint16_t)off;
            map->entries[idx].ivar_name  = ivar_getName(ivars[i]);
            map->entry_count++;

            /* Insert into hash table. */
            uint32_t slot = hash_ivar(cls_name, (uint16_t)off) & map->slot_mask;
            while (map->slots[slot].entry_idx != 0) {
                slot = (slot + 1) & map->slot_mask;
            }
            map->slots[slot].entry_idx = idx + 1;  /* 1-based */
        }
        free(ivars);
    }

    return map;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

/* Direct lookup for a specific class name pointer and offset. */
static const char *lookup_direct(const mg_ivar_map_t *map,
                                 const char *cls_name,
                                 uint16_t offset) {
    uint32_t slot = hash_ivar(cls_name, offset) & map->slot_mask;

    while (map->slots[slot].entry_idx != 0) {
        uint32_t idx = map->slots[slot].entry_idx - 1;
        const ivar_entry_t *e = &map->entries[idx];
        if (e->class_name == cls_name && e->offset == offset)
            return e->ivar_name;
        slot = (slot + 1) & map->slot_mask;
    }
    return NULL;
}

const char *mg_ivar_map_lookup(const mg_ivar_map_t *map,
                               const char *class_name,
                               uint16_t byte_offset) {
    if (!map || !class_name) return NULL;

    /* Walk superclass chain to find the declaring class. */
    Class cls = objc_getClass(class_name);
    while (cls) {
        const char *name = class_getName(cls);
        const char *result = lookup_direct(map, name, byte_offset);
        if (result) return result;
        cls = class_getSuperclass(cls);
    }
    return NULL;
}

#endif /* MG_APP_STORE_COMPLIANT */
