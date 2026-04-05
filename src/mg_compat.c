/*
 * mg_compat.c — App Store compliant alternatives
 *
 * Compiled only when MG_APP_STORE_COMPLIANT is defined.
 * Provides degraded but functional implementations that avoid
 * Mach APIs in the grey area (task_threads, thread_suspend,
 * mach_vm_region_recurse, malloc zone enumerator).
 */

#ifdef MG_APP_STORE_COMPLIANT

#include "mg_compat.h"
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Compliant VM region collection                                      */
/* ------------------------------------------------------------------ */

int mg_vm_region_collect_compat(mg_pool_t *pool, mg_vm_snapshot_t *out) {
    if (!pool || !out) return -1;
    memset(out, 0, sizeof(*out));

    /*
     * Without mach_vm_region_recurse, we can only get aggregate
     * memory info from task_info.
     */
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(),
                                TASK_VM_INFO,
                                (task_info_t)&vm_info,
                                &count);
    if (kr != KERN_SUCCESS) return -1;

    /* Create a single synthetic region representing the whole task. */
    mg_vm_region_t *region = (mg_vm_region_t *)mg_pool_alloc(
        pool, sizeof(mg_vm_region_t));
    if (!region) return -1;

    memset(region, 0, sizeof(*region));
    region->virtual_size  = (uint64_t)vm_info.virtual_size;
    region->resident_size = (uint64_t)vm_info.resident_size;

    out->regions        = region;
    out->region_count   = 1;
    out->total_virtual  = region->virtual_size;
    out->total_resident = region->resident_size;

    /* No per-tag breakdown available. */
    out->tag_stats = NULL;
    out->tag_count = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Compliant heap collection                                           */
/* ------------------------------------------------------------------ */

int mg_heap_collect_compat(mg_pool_t *pool,
                           uint32_t top_n,
                           mg_heap_snapshot_t *out) {
    if (!pool || !out) return -1;
    memset(out, 0, sizeof(*out));
    (void)top_n;

    /*
     * Use malloc_zone_statistics for per-zone aggregate data.
     * We cannot enumerate individual allocations or identify
     * ObjC objects in this mode.
     */
    vm_address_t *zone_addrs = NULL;
    unsigned zone_count = 0;

    /* malloc_get_all_zones is technically Mach-based but is
     * widely used and generally accepted by App Review. */
    kern_return_t kr = malloc_get_all_zones(
        mach_task_self(), NULL, &zone_addrs, &zone_count);

    if (kr != KERN_SUCCESS) {
        /* Fallback: just report aggregate from task_info. */
        task_vm_info_data_t vm_info;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&vm_info, &count);
        out->total_allocated = (uint64_t)vm_info.internal;
        return 0;
    }

    if (zone_count > MG_MAX_ZONES) zone_count = MG_MAX_ZONES;

    for (unsigned z = 0; z < zone_count; z++) {
        malloc_zone_t *zone = (malloc_zone_t *)zone_addrs[z];
        malloc_statistics_t stats;
        malloc_zone_statistics(zone, &stats);

        out->zones[z].name      = malloc_get_zone_name(zone);
        out->zones[z].allocated = stats.size_in_use;
        out->zones[z].blocks    = stats.blocks_in_use;

        out->total_allocated += stats.size_in_use;
        out->total_blocks    += stats.blocks_in_use;
    }
    out->zone_count = zone_count;

    /* No ObjC object identification in compliant mode. */
    out->total_objects   = 0;
    out->raw_allocations = out->total_blocks;
    out->top_classes     = NULL;
    out->top_count       = 0;

    return 0;
}

#else
/* Empty translation unit when not in compliant mode. */
typedef int mg_compat_unused_t;
#endif /* MG_APP_STORE_COMPLIANT */
