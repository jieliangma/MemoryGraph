/*
 * mg_ivar_map.h — ObjC ivar offset-to-name lookup table
 *
 * Built before thread suspension using ObjC runtime introspection.
 * Used after thread resumption to resolve ivar names in retain cycles.
 *
 * All persistent storage comes from the pre-allocated pool.
 */

#ifndef MG_IVAR_MAP_H
#define MG_IVAR_MAP_H

#include "mg_pool.h"
#include <stdint.h>

/* Opaque ivar map handle. */
typedef struct mg_ivar_map mg_ivar_map_t;

/*
 * Build the ivar map by introspecting all loaded ObjC classes.
 * Only records ivars with object-type encoding (@).
 *
 * Uses malloc internally (via class_copyIvarList) — must NOT be
 * called while threads are suspended.
 *
 * Returns NULL on allocation failure.
 */
mg_ivar_map_t *mg_ivar_map_build(mg_pool_t *pool);

/*
 * Look up the ivar name for a given class and byte offset.
 * Walks the superclass chain if the ivar is inherited.
 *
 * Returns the ivar name (interned string from ObjC runtime, stable),
 * or NULL if no matching object-typed ivar exists at that offset.
 */
const char *mg_ivar_map_lookup(const mg_ivar_map_t *map,
                               const char *class_name,
                               uint16_t byte_offset);

#endif /* MG_IVAR_MAP_H */
