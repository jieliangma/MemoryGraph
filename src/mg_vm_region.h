/*
 * mg_vm_region.h — Virtual memory region enumeration
 *
 * Walks all VM regions via mach_vm_region_recurse() and records
 * each region's tag, size, protection, and share mode.
 */

#ifndef MG_VM_REGION_H
#define MG_VM_REGION_H

#include "mg_pool.h"
#include <stdint.h>

/* One VM region entry. */
typedef struct {
    uint64_t address;
    uint64_t virtual_size;
    uint64_t resident_size;
    uint32_t user_tag;
    uint8_t  protection;   /* VM_PROT_READ=1 | WRITE=2 | EXECUTE=4 */
    uint8_t  share_mode;
} mg_vm_region_t;

/* Per-tag aggregation. */
typedef struct {
    uint32_t tag;
    uint32_t region_count;
    uint64_t total_virtual;
    uint64_t total_resident;
} mg_vm_tag_stat_t;

/* Snapshot of all VM regions. */
typedef struct {
    mg_vm_region_t  *regions;
    uint32_t         region_count;
    uint64_t         total_virtual;
    uint64_t         total_resident;

    mg_vm_tag_stat_t *tag_stats;    /* sorted by total_resident desc */
    uint32_t          tag_count;
} mg_vm_snapshot_t;

/*
 * Enumerate all VM regions of the current task.
 * Results are allocated from `pool`.
 * Returns 0 on success, negative on error.
 */
int mg_vm_region_collect(mg_pool_t *pool, mg_vm_snapshot_t *out);

/*
 * Return a human-readable name for a VM_MEMORY_* tag.
 * Returns a static string — never NULL.
 */
const char *mg_vm_tag_name(uint32_t tag);

/*
 * Return a string like "rw-" for a protection bitmask.
 * Writes into `buf` (must be >= 4 bytes). Returns `buf`.
 */
char *mg_vm_prot_string(uint8_t prot, char buf[4]);

/*
 * Return a human-readable share mode name.
 */
const char *mg_vm_share_mode_name(uint8_t mode);

#endif /* MG_VM_REGION_H */
