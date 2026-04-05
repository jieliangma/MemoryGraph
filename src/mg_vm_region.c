/*
 * mg_vm_region.c — Virtual memory region enumeration
 *
 * Uses vm_region_recurse_64() to walk every region in the current
 * task.  Results are stored in the pre-allocated pool.
 *
 * Note: mach_vm_region_recurse / mach_vm.h is unavailable on the
 * iOS Simulator SDK.  vm_region_recurse_64 is equivalent on 64-bit
 * platforms (all supported targets are arm64).
 */

#include "mg_vm_region.h"
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <string.h>
#include <stdlib.h>  /* qsort */

/* ------------------------------------------------------------------ */
/* Tag name lookup                                                     */
/* ------------------------------------------------------------------ */

const char *mg_vm_tag_name(uint32_t tag) {
    switch (tag) {
    case 0:                                 return "NONE";
    case VM_MEMORY_MALLOC:                  return "MALLOC";
    case VM_MEMORY_MALLOC_SMALL:            return "MALLOC_SMALL";
    case VM_MEMORY_MALLOC_LARGE:            return "MALLOC_LARGE";
    case VM_MEMORY_MALLOC_HUGE:             return "MALLOC_HUGE";
    case VM_MEMORY_SBRK:                    return "SBRK";
    case VM_MEMORY_REALLOC:                 return "REALLOC";
    case VM_MEMORY_MALLOC_TINY:             return "MALLOC_TINY";
    case VM_MEMORY_MALLOC_LARGE_REUSABLE:   return "MALLOC_LARGE_REUSABLE";
    case VM_MEMORY_MALLOC_LARGE_REUSED:     return "MALLOC_LARGE_REUSED";
    case VM_MEMORY_ANALYSIS_TOOL:           return "ANALYSIS_TOOL";
    case VM_MEMORY_MALLOC_NANO:             return "MALLOC_NANO";
    case VM_MEMORY_MACH_MSG:                return "MACH_MSG";
    case VM_MEMORY_IOKIT:                   return "IOKIT";
    case VM_MEMORY_STACK:                   return "STACK";
    case VM_MEMORY_GUARD:                   return "GUARD";
    case VM_MEMORY_SHARED_PMAP:             return "SHARED_PMAP";
    case VM_MEMORY_DYLIB:                   return "DYLIB";
    case VM_MEMORY_OBJC_DISPATCHERS:        return "OBJC_DISPATCHERS";
    case VM_MEMORY_UNSHARED_PMAP:           return "UNSHARED_PMAP";
    case VM_MEMORY_APPKIT:                  return "APPKIT";
    case VM_MEMORY_FOUNDATION:              return "FOUNDATION";
    case VM_MEMORY_COREGRAPHICS:            return "COREGRAPHICS";
    case VM_MEMORY_CORESERVICES:            return "CORESERVICES";
    case VM_MEMORY_JAVA:                    return "JAVA";
    case VM_MEMORY_COREDATA:                return "COREDATA";
    case VM_MEMORY_COREDATA_OBJECTIDS:      return "COREDATA_OBJECTIDS";
    case VM_MEMORY_DYLD:                    return "DYLD";
    case VM_MEMORY_DYLD_MALLOC:             return "DYLD_MALLOC";
    case VM_MEMORY_SQLITE:                  return "SQLITE";
    case VM_MEMORY_JAVASCRIPT_CORE:         return "JAVASCRIPT_CORE";
    case VM_MEMORY_JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR:
                                            return "JSC_JIT_EXEC";
    case VM_MEMORY_JAVASCRIPT_JIT_REGISTER_FILE:
                                            return "JSC_JIT_REGFILE";
    case VM_MEMORY_GLSL:                    return "GLSL";
    case VM_MEMORY_OPENCL:                  return "OPENCL";
    case VM_MEMORY_COREIMAGE:               return "COREIMAGE";
    case VM_MEMORY_WEBCORE_PURGEABLE_BUFFERS:
                                            return "WEBCORE_PURGEABLE";
    case VM_MEMORY_IMAGEIO:                 return "IMAGEIO";
    case VM_MEMORY_COREPROFILE:             return "COREPROFILE";
    case VM_MEMORY_ASSETSD:                 return "ASSETSD";
    case VM_MEMORY_OS_ALLOC_ONCE:           return "OS_ALLOC_ONCE";
    case VM_MEMORY_LIBDISPATCH:             return "LIBDISPATCH";
    case VM_MEMORY_ACCELERATE:              return "ACCELERATE";
    case VM_MEMORY_COREUI:                  return "COREUI";
    case VM_MEMORY_COREUIFILE:              return "COREUIFILE";
    case VM_MEMORY_GENEALOGY:               return "GENEALOGY";
    case VM_MEMORY_RAWCAMERA:               return "RAWCAMERA";
    case VM_MEMORY_CORPSEINFO:              return "CORPSEINFO";
    case VM_MEMORY_ASL:                     return "ASL";
    case VM_MEMORY_SWIFT_RUNTIME:           return "SWIFT_RUNTIME";
    case VM_MEMORY_SWIFT_METADATA:          return "SWIFT_METADATA";
    case VM_MEMORY_DHMM:                    return "DHMM";
    case VM_MEMORY_SCENEKIT:                return "SCENEKIT";
    case VM_MEMORY_SKYWALK:                 return "SKYWALK";
    case VM_MEMORY_IOSURFACE:               return "IOSURFACE";
    case VM_MEMORY_LIBNETWORK:              return "LIBNETWORK";
    case VM_MEMORY_AUDIO:                   return "AUDIO";
    case VM_MEMORY_VIDEOBITSTREAM:           return "VIDEOBITSTREAM";
    default:                                return "UNKNOWN";
    }
}

char *mg_vm_prot_string(uint8_t prot, char buf[4]) {
    buf[0] = (prot & VM_PROT_READ)    ? 'r' : '-';
    buf[1] = (prot & VM_PROT_WRITE)   ? 'w' : '-';
    buf[2] = (prot & VM_PROT_EXECUTE) ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

const char *mg_vm_share_mode_name(uint8_t mode) {
    switch (mode) {
    case SM_COW:              return "cow";
    case SM_PRIVATE:          return "private";
    case SM_EMPTY:            return "empty";
    case SM_SHARED:           return "shared";
    case SM_TRUESHARED:       return "trueshared";
    case SM_PRIVATE_ALIASED:  return "private_aliased";
    case SM_SHARED_ALIASED:   return "shared_aliased";
    default:                  return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Tag aggregation (fixed-size array indexed by tag value)              */
/* ------------------------------------------------------------------ */

#define MG_MAX_TAG_VALUE 256

typedef struct {
    uint32_t count;
    uint64_t total_virtual;
    uint64_t total_resident;
} tag_acc_t;

static int cmp_tag_stat_desc(const void *a, const void *b) {
    const mg_vm_tag_stat_t *sa = (const mg_vm_tag_stat_t *)a;
    const mg_vm_tag_stat_t *sb = (const mg_vm_tag_stat_t *)b;
    if (sa->total_resident > sb->total_resident) return -1;
    if (sa->total_resident < sb->total_resident) return  1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

/*
 * First pass: count regions so we can allocate the right amount
 * from the pool.  This avoids over-allocating or running out.
 */
static uint32_t count_regions(void) {
    mach_port_t task = mach_task_self();
    vm_address_t addr = 0;
    vm_size_t size = 0;
    natural_t depth = 0;
    uint32_t count = 0;

    while (1) {
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = vm_region_recurse_64(
            task, &addr, &size, &depth,
            (vm_region_recurse_info_64_t)&info, &info_count);

        if (kr != KERN_SUCCESS) break;

        if (info.is_submap) {
            depth++;
            continue;
        }
        count++;
        addr += size;
    }
    return count;
}

int mg_vm_region_collect(mg_pool_t *pool, mg_vm_snapshot_t *out) {
    if (!pool || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Count regions first, with 10% padding for growth between passes. */
#define MG_VM_REGION_PAD_PERCENT 10
    uint32_t est = count_regions();
    if (est == 0) return -1;
    est += est / MG_VM_REGION_PAD_PERCENT;

    /* Allocate region array from pool. */
    mg_vm_region_t *regions = (mg_vm_region_t *)mg_pool_alloc(
        pool, sizeof(mg_vm_region_t) * est);
    if (!regions) return -1;

    /* Tag accumulator — stack allocated, indexed by tag value. */
    tag_acc_t tags[MG_MAX_TAG_VALUE];
    memset(tags, 0, sizeof(tags));

    /* Second pass: fill region data. */
    mach_port_t task = mach_task_self();
    vm_size_t page_size = vm_page_size;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    natural_t depth = 0;
    uint32_t idx = 0;
    uint64_t total_virtual = 0;
    uint64_t total_resident = 0;

    while (idx < est) {
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;

        kern_return_t kr = vm_region_recurse_64(
            task, &addr, &size, &depth,
            (vm_region_recurse_info_64_t)&info, &info_count);

        if (kr != KERN_SUCCESS) break;

        if (info.is_submap) {
            depth++;
            continue;
        }

        uint64_t resident = (uint64_t)info.pages_resident * page_size;

        regions[idx].address       = (uint64_t)addr;
        regions[idx].virtual_size  = (uint64_t)size;
        regions[idx].resident_size = resident;
        regions[idx].user_tag      = info.user_tag;
        regions[idx].protection    = (uint8_t)info.protection;
        regions[idx].share_mode    = (uint8_t)info.share_mode;

        total_virtual  += size;
        total_resident += resident;

        /* Accumulate per-tag stats. */
        uint32_t tag = info.user_tag;
        if (tag < MG_MAX_TAG_VALUE) {
            tags[tag].count++;
            tags[tag].total_virtual  += size;
            tags[tag].total_resident += resident;
        }

        idx++;
        addr += size;
    }

    /* Count distinct tags. */
    uint32_t tag_count = 0;
    for (uint32_t t = 0; t < MG_MAX_TAG_VALUE; t++) {
        if (tags[t].count > 0) tag_count++;
    }

    /* Allocate tag summary array from pool. */
    mg_vm_tag_stat_t *tag_stats = NULL;
    if (tag_count > 0) {
        tag_stats = (mg_vm_tag_stat_t *)mg_pool_alloc(
            pool, sizeof(mg_vm_tag_stat_t) * tag_count);
        if (tag_stats) {
            uint32_t ti = 0;
            for (uint32_t t = 0; t < MG_MAX_TAG_VALUE && ti < tag_count; t++) {
                if (tags[t].count == 0) continue;
                tag_stats[ti].tag            = t;
                tag_stats[ti].region_count   = tags[t].count;
                tag_stats[ti].total_virtual  = tags[t].total_virtual;
                tag_stats[ti].total_resident = tags[t].total_resident;
                ti++;
            }
            /* Sort by total_resident descending. */
            qsort(tag_stats, tag_count, sizeof(mg_vm_tag_stat_t),
                  cmp_tag_stat_desc);
        }
    }

    out->regions        = regions;
    out->region_count   = idx;
    out->total_virtual  = total_virtual;
    out->total_resident = total_resident;
    out->tag_stats      = tag_stats;
    out->tag_count      = tag_count;
    return 0;
}
