/*
 * mg_report.c — Binary report writer (.mgbin)
 *
 * Writes a compact binary OOM diagnostic report using the
 * MGRP format.  No malloc during write — all memory from pool.
 */

#include "mg_report.h"
#include "mg_binary.h"
#include "mg_vm_region.h"
#include "mg_heap.h"
#include "../include/memory_graph.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <TargetConditionals.h>

#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
#include <os/proc.h>
#endif

/* ================================================================== */
/* Binary report writer                                                */
/* ================================================================== */

/* Encode trigger reason string to uint8. */
static uint8_t encode_trigger_reason(const char *reason) {
    if (!reason) return MG_TRIGGER_MANUAL;
    if (strcmp(reason, "threshold") == 0) return MG_TRIGGER_THRESHOLD;
    if (strcmp(reason, "critical") == 0)  return MG_TRIGGER_CRITICAL;
    return MG_TRIGGER_MANUAL;
}

/* Build string table from all data sections. */
static bool build_string_table(mg_string_table_t *t,
                               mg_pool_t *pool,
                               const mg_report_data_t *data,
                               const char *device_model,
                               const char *device_os_ver) {
    /* Estimate: ~200 unique strings max. */
    if (!mg_string_table_init(t, pool, 512)) return false;

    /* Version string. */
    mg_string_table_add(t, MEMORY_GRAPH_VERSION);

    /* Device info. */
    mg_string_table_add(t, device_model);
    mg_string_table_add(t, device_os_ver);

    /* VM tag names + summary. */
    if (data->vm_snap) {
        for (uint32_t i = 0; i < data->vm_snap->region_count; i++) {
            mg_string_table_add(t,
                mg_vm_tag_name(data->vm_snap->regions[i].user_tag));
        }
        for (uint32_t i = 0; i < data->vm_snap->tag_count; i++) {
            mg_string_table_add(t,
                mg_vm_tag_name(data->vm_snap->tag_stats[i].tag));
        }
    }

    /* Heap zone names + class names. */
    if (data->heap_snap) {
        for (uint32_t i = 0; i < data->heap_snap->zone_count; i++) {
            if (data->heap_snap->zones[i].name) {
                mg_string_table_add(t, data->heap_snap->zones[i].name);
            }
        }
        for (uint32_t i = 0; i < data->heap_snap->top_count; i++) {
            mg_string_table_add(t, data->heap_snap->top_classes[i].class_name);
        }
    }

    /* Leak suspect class names. */
    if (data->leak_result) {
        for (uint32_t i = 0; i < data->leak_result->suspect_count; i++) {
            mg_string_table_add(t, data->leak_result->suspects[i].class_name);
        }
    }

    /* Reachability suspect class names. */
    if (data->reach_result) {
        for (uint32_t i = 0; i < data->reach_result->suspect_count; i++) {
            mg_string_table_add(t, data->reach_result->suspects[i].class_name);
        }
    }

    /* Ref graph cycle class + ivar names. */
    if (data->ref_result && data->ref_result->enabled) {
        for (uint32_t i = 0; i < data->ref_result->cycle_count; i++) {
            const mg_retain_cycle_t *cyc = &data->ref_result->cycles[i];
            for (uint32_t j = 0; j < cyc->length; j++) {
                mg_string_table_add(t, cyc->steps[j].from_class);
                mg_string_table_add(t, cyc->steps[j].ivar_name);
                mg_string_table_add(t, cyc->steps[j].to_class);
            }
        }
    }

    return true;
}


static void write_metadata_binary(mg_binary_writer_t *w,
                                  const mg_report_data_t *data,
                                  const mg_string_table_t *t) {
    /* Payload: i64(8) + u8(1) + u16(2) = 11 bytes. */
    mg_binary_section_header(w, MG_SEC_METADATA, 11);
    mg_binary_i64(w, (int64_t)time(NULL));
    mg_binary_u8(w, encode_trigger_reason(data->trigger_reason));
    mg_binary_u16(w, mg_string_table_add(
        (mg_string_table_t *)t, MEMORY_GRAPH_VERSION));
}

static void write_device_binary(mg_binary_writer_t *w,
                                const mg_string_table_t *t,
                                const char *model,
                                const char *os_ver) {
    uint16_t model_idx  = mg_string_table_find(t, model);
    uint16_t os_ver_idx = mg_string_table_find(t, os_ver);

    uint64_t phys = 0;
    size_t len = sizeof(phys);
    sysctlbyname("hw.memsize", &phys, &len, NULL, 0);

    /* Payload: u16(2) + u16(2) + u64(8) = 12 bytes. */
    mg_binary_section_header(w, MG_SEC_DEVICE, 12);
    mg_binary_u16(w, model_idx);
    mg_binary_u16(w, os_ver_idx);
    mg_binary_u64(w, phys);
}

static void write_app_binary(mg_binary_writer_t *w) {
    uint64_t footprint = 0;
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                (task_info_t)&vm_info, &count);
    if (kr == KERN_SUCCESS) {
        footprint = (uint64_t)vm_info.phys_footprint;
    }

    uint64_t available = 0;
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
    available = (uint64_t)os_proc_available_memory();
#endif

    /* Payload: u64(8) + u64(8) = 16 bytes. */
    mg_binary_section_header(w, MG_SEC_APP, 16);
    mg_binary_u64(w, footprint);
    mg_binary_u64(w, available);
}

static void write_vm_summary_binary(mg_binary_writer_t *w,
                                    const mg_vm_snapshot_t *snap,
                                    const mg_string_table_t *t) {
    /* Payload: u64(8) + u64(8) + u16(2) + tag_count * 14 */
    uint32_t payload = 18 + (uint32_t)snap->tag_count * 14;
    mg_binary_section_header(w, MG_SEC_VM_SUMMARY, payload);

    mg_binary_u64(w, snap->total_virtual);
    mg_binary_u64(w, snap->total_resident);
    mg_binary_u16(w, (uint16_t)snap->tag_count);

    for (uint32_t i = 0; i < snap->tag_count; i++) {
        const mg_vm_tag_stat_t *ts = &snap->tag_stats[i];
        mg_binary_u16(w, mg_string_table_find(t, mg_vm_tag_name(ts->tag)));
        mg_binary_u32(w, ts->region_count);
        mg_binary_u64(w, ts->total_resident);
    }
}

static void write_vm_regions_binary(mg_binary_writer_t *w,
                                    const mg_vm_snapshot_t *snap,
                                    const mg_string_table_t *t) {
    /* Payload: u32(4) + region_count * 28 */
    uint32_t payload = 4 + snap->region_count * 28;
    mg_binary_section_header(w, MG_SEC_VM_REGIONS, payload);

    mg_binary_u32(w, snap->region_count);

    for (uint32_t i = 0; i < snap->region_count; i++) {
        const mg_vm_region_t *r = &snap->regions[i];
        mg_binary_u64(w, r->address);
        mg_binary_u64(w, r->virtual_size);
        mg_binary_u64(w, r->resident_size);

        mg_binary_u16(w, mg_string_table_find(t, mg_vm_tag_name(r->user_tag)));
        mg_binary_u8(w, r->protection);
        mg_binary_u8(w, r->share_mode);
    }
}

static void write_heap_binary(mg_binary_writer_t *w,
                              const mg_heap_snapshot_t *snap,
                              const mg_string_table_t *t) {
    /* Count actual zones (skip NULL names). */
    uint8_t actual_zones = 0;
    for (uint32_t i = 0; i < snap->zone_count; i++) {
        if (snap->zones[i].name) actual_zones++;
    }

    /* Payload: u64*3(24) + u8(1) + actual_zones*18 + u16(2) + top_count*14 */
    uint32_t payload = 27 + (uint32_t)actual_zones * 18 +
                       (uint32_t)snap->top_count * 14;
    mg_binary_section_header(w, MG_SEC_HEAP, payload);

    mg_binary_u64(w, snap->total_allocated);
    mg_binary_u64(w, snap->total_blocks);
    mg_binary_u64(w, snap->total_objects);
    mg_binary_u8(w, actual_zones);

    for (uint32_t i = 0; i < snap->zone_count; i++) {
        if (!snap->zones[i].name) continue;
        mg_binary_u16(w, mg_string_table_find(t, snap->zones[i].name));
        mg_binary_u64(w, snap->zones[i].allocated);
        mg_binary_u64(w, snap->zones[i].blocks);
    }

    mg_binary_u16(w, (uint16_t)snap->top_count);
    for (uint32_t i = 0; i < snap->top_count; i++) {
        const mg_class_stat_t *cs = &snap->top_classes[i];
        mg_binary_u16(w, mg_string_table_find(t, cs->class_name));
        mg_binary_u32(w, cs->count);
        mg_binary_u64(w, cs->total_size);
    }
}

static void write_raw_alloc_binary(mg_binary_writer_t *w,
                                   const mg_heap_snapshot_t *snap) {
    /* Payload: u64(8) + u32(4) + u32*6(24) = 36 bytes */
    uint32_t payload = 8 + 4 + MG_RAW_BUCKET_COUNT * 4;
    mg_binary_section_header(w, MG_SEC_RAW_ALLOC, payload);

    mg_binary_u64(w, snap->raw_stat.raw_total_size);
    mg_binary_u32(w, (uint32_t)snap->raw_allocations);
    for (int i = 0; i < MG_RAW_BUCKET_COUNT; i++) {
        mg_binary_u32(w, snap->raw_stat.raw_size_buckets[i]);
    }
}

static void write_leak_binary(mg_binary_writer_t *w,
                              const mg_leak_result_t *leak,
                              const mg_string_table_t *t) {
    uint32_t sc = leak ? leak->suspect_count : 0;
    /* Payload: u16(2) + sc * 15 */
    uint32_t payload = 2 + sc * 15;
    mg_binary_section_header(w, MG_SEC_LEAK, payload);

    mg_binary_u16(w, (uint16_t)sc);
    for (uint32_t i = 0; i < sc; i++) {
        const mg_leak_suspect_t *s = &leak->suspects[i];
        mg_binary_u16(w, mg_string_table_find(t, s->class_name));
        mg_binary_u32(w, s->count);
        mg_binary_u64(w, s->total_size);
        mg_binary_u8(w, (uint8_t)s->reason);
    }
}

static void write_reachability_binary(mg_binary_writer_t *w,
                                      const mg_reach_result_t *reach,
                                      const mg_string_table_t *t) {
    uint32_t sc = reach ? reach->suspect_count : 0;
    /* Payload: u32(4)*3 + u16(2) + sc * 14 */
    uint32_t payload = 14 + sc * 14;
    mg_binary_section_header(w, MG_SEC_REACHABILITY, payload);

    mg_binary_u32(w, reach ? reach->total_reachable   : 0);
    mg_binary_u32(w, reach ? reach->total_unreachable : 0);
    mg_binary_u32(w, reach ? reach->root_count        : 0);
    mg_binary_u16(w, (uint16_t)sc);
    for (uint32_t i = 0; i < sc; i++) {
        const mg_reach_suspect_t *s = &reach->suspects[i];
        mg_binary_u16(w, mg_string_table_find(t, s->class_name));
        mg_binary_u32(w, s->count);
        mg_binary_u64(w, s->total_size);
    }
}

static void write_ref_graph_binary(mg_binary_writer_t *w,
                                   const mg_ref_graph_result_t *ref,
                                   const mg_string_table_t *t) {
    /* Compute payload: 30 fixed + variable cycles.
     * v2: each step is 7 bytes (from_class u16 + edge_type u8 + ivar_name u16 + to_class u16). */
    uint32_t payload = 30;
    for (uint32_t i = 0; i < ref->cycle_count; i++) {
        payload += 14 + ref->cycles[i].length * 7;
    }

    mg_binary_section_header(w, MG_SEC_REF_GRAPH, payload);

    mg_binary_u32(w, ref->total_nodes);
    mg_binary_u32(w, ref->total_edges);
    mg_binary_u32(w, ref->objects_dropped);
    mg_binary_u32(w, ref->edges_dropped);
    mg_binary_u32(w, ref->cycles_dropped_length);
    mg_binary_u32(w, ref->cycles_dropped_limit);
    mg_binary_u32(w, ref->system_cycles_filtered);
    mg_binary_u16(w, (uint16_t)ref->cycle_count);

    for (uint32_t i = 0; i < ref->cycle_count; i++) {
        const mg_retain_cycle_t *cyc = &ref->cycles[i];
        mg_binary_u8(w, (uint8_t)cyc->length);
        mg_binary_u32(w, cyc->instance_count);
        mg_binary_u64(w, cyc->total_size);
        mg_binary_u8(w, cyc->is_app_cycle ? 1 : 0);

        for (uint32_t j = 0; j < cyc->length; j++) {
            mg_binary_u16(w, mg_string_table_find(t, cyc->steps[j].from_class));
            mg_binary_u8(w, (uint8_t)cyc->steps[j].edge_type);
            mg_binary_u16(w, mg_string_table_find(t, cyc->steps[j].ivar_name));
            mg_binary_u16(w, mg_string_table_find(t, cyc->steps[j].to_class));
        }
    }
}

int mg_report_write(const char *path, const mg_report_data_t *data,
                           mg_pool_t *pool) {
    if (!path || !data || !pool) return MG_ERR_INVALID_ARG;

    /* Resolve device info once — shared between string table and writer. */
    struct utsname u;
    const char *device_model  = "unknown";
    const char *device_os_ver = "unknown";
    if (uname(&u) == 0) {
        device_model  = u.machine;
        device_os_ver = u.release;
    }

    /* Build string table (includes device strings). */
    mg_string_table_t strtab;
    if (!build_string_table(&strtab, pool, data,
                            device_model, device_os_ver))
        return MG_ERR_ALLOC;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return MG_ERR_IO;

    mg_binary_writer_t w;
    mg_binary_init(&w, fd);

    /* Count sections. */
    uint32_t section_count = 7;  /* strtab, meta, device, app, vm_sum, vm_reg, heap */
    if (data->heap_snap) section_count++;  /* raw_alloc */
    section_count++;             /* leak (always present, may be empty) */
    if (data->ref_result && data->ref_result->enabled) {
        section_count++;
    }
    if (data->reach_result && data->reach_result->suspect_count > 0) {
        section_count++;
    }

    /* File header (16 bytes). */
    uint16_t flags = 0;
    if (data->truncated) flags |= MG_FLAG_TRUNCATED;
    if (data->ref_result && data->ref_result->enabled) {
        flags |= MG_FLAG_REF_GRAPH_ENABLED;
    }

    mg_binary_u32(&w, MG_BINARY_MAGIC);
    mg_binary_u16(&w, MG_BINARY_VERSION);
    mg_binary_u16(&w, flags);
    mg_binary_u32(&w, section_count);
    mg_binary_u32(&w, 0);  /* reserved */

    /* String table must be first. */
    mg_binary_write_string_table(&w, &strtab);

    /* Data sections. */
    write_metadata_binary(&w, data, &strtab);
    write_device_binary(&w, &strtab, device_model, device_os_ver);
    write_app_binary(&w);

    if (data->vm_snap) {
        write_vm_summary_binary(&w, data->vm_snap, &strtab);
        write_vm_regions_binary(&w, data->vm_snap, &strtab);
    }

    if (data->heap_snap) {
        write_heap_binary(&w, data->heap_snap, &strtab);
        write_raw_alloc_binary(&w, data->heap_snap);
    }

    write_leak_binary(&w, data->leak_result, &strtab);

    if (data->ref_result && data->ref_result->enabled) {
        write_ref_graph_binary(&w, data->ref_result, &strtab);
    }

    if (data->reach_result && data->reach_result->suspect_count > 0) {
        write_reachability_binary(&w, data->reach_result, &strtab);
    }

    bool ok = mg_binary_finish(&w);
    close(fd);
    return ok ? MG_OK : MG_ERR_IO;
}

/* ================================================================== */
/* Text conversion (public API)                                        */
/* ================================================================== */

#include "mg_binary_reader.h"

int mg_report_to_text(const char *report_path, const char *output_path) {
    if (!report_path || !output_path) return -1;

    int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    int ret = mg_report_read_text(report_path, fd);
    close(fd);
    return ret;
}
