/*
 * mg_compat.h — App Store compliant alternatives
 *
 * When MG_APP_STORE_COMPLIANT is defined, these functions
 * replace the Mach-API-based implementations with alternatives
 * that only use public APIs.
 *
 * Trade-offs:
 *   - VM region info is limited (no per-region detail)
 *   - Heap enumeration uses zone statistics instead of
 *     per-allocation walking
 *   - ObjC object identification still works (public API)
 */

#ifndef MG_COMPAT_H
#define MG_COMPAT_H

#ifdef MG_APP_STORE_COMPLIANT

#include "mg_pool.h"
#include "mg_vm_region.h"
#include "mg_heap.h"

/*
 * Compliant VM region collection.
 * Uses task_info for aggregate stats only — no per-region breakdown.
 */
int mg_vm_region_collect_compat(mg_pool_t *pool, mg_vm_snapshot_t *out);

/*
 * Compliant heap collection.
 * Uses malloc_zone_statistics for per-zone stats.
 * Still uses ObjC runtime (public API) for object identification
 * but cannot enumerate individual allocations.
 */
int mg_heap_collect_compat(mg_pool_t *pool,
                           uint32_t top_n,
                           mg_heap_snapshot_t *out);

#endif /* MG_APP_STORE_COMPLIANT */
#endif /* MG_COMPAT_H */
