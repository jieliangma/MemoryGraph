/*
 * mg_binary_reader.c — MGRP binary reader + text formatter
 *
 * Reads a .mgbin file into malloc'd memory, parses sections,
 * and writes human-readable text to an output fd.
 *
 * This is an OFFLINE tool — uses malloc freely.
 * Never called during OOM collection.
 */

#include "mg_binary_reader.h"
#include "mg_binary.h"
#include "../include/memory_graph.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* Reader state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} mg_reader_t;

static bool reader_has(const mg_reader_t *r, size_t n) {
    return r->pos + n <= r->size;
}

static uint8_t read_u8(mg_reader_t *r) {
    if (!reader_has(r, 1)) return 0;
    return r->data[r->pos++];
}

static uint16_t read_u16(mg_reader_t *r) {
    if (!reader_has(r, 2)) return 0;
    uint16_t v;
    memcpy(&v, r->data + r->pos, 2);
    r->pos += 2;
    return v;
}

static uint32_t read_u32(mg_reader_t *r) {
    if (!reader_has(r, 4)) return 0;
    uint32_t v;
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static uint64_t read_u64(mg_reader_t *r) {
    if (!reader_has(r, 8)) return 0;
    uint64_t v;
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

static int64_t read_i64(mg_reader_t *r) {
    if (!reader_has(r, 8)) return 0;
    int64_t v;
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

/* ------------------------------------------------------------------ */
/* String table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char   **strings;     /* malloc'd array of malloc'd strings */
    uint16_t count;
} read_string_table_t;

static bool parse_string_table(mg_reader_t *r, uint32_t payload_len,
                               read_string_table_t *st) {
    size_t end = r->pos + payload_len;

    st->count = read_u16(r);
    st->strings = calloc(st->count, sizeof(char *));
    if (!st->strings) return false;

    for (uint16_t i = 0; i < st->count; i++) {
        uint16_t len = read_u16(r);
        if (!reader_has(r, len)) { st->count = i; return false; }

        st->strings[i] = malloc(len + 1);
        if (!st->strings[i]) { st->count = i; return false; }
        memcpy(st->strings[i], r->data + r->pos, len);
        st->strings[i][len] = '\0';
        r->pos += len;
    }

    r->pos = end;   /* skip any padding */
    return true;
}

static const char *strtab_get(const read_string_table_t *st, uint16_t idx) {
    if (idx >= st->count) return "<?>";
    return st->strings[idx];
}

static void free_string_table(read_string_table_t *st) {
    for (uint16_t i = 0; i < st->count; i++) {
        free(st->strings[i]);
    }
    free(st->strings);
    st->strings = NULL;
    st->count = 0;
}

/* ------------------------------------------------------------------ */
/* Size formatting                                                     */
/* ------------------------------------------------------------------ */

static void fmt_size(char *buf, size_t bufsz, uint64_t bytes) {
    if (bytes >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(buf, bufsz, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, bufsz, "%" PRIu64 " B", bytes);
    }
}

/* Buffered output writer — avoids many small write() calls. */
typedef struct {
    int    fd;
    char   buf[8192];
    size_t pos;
} out_writer_t;

static void out_flush(out_writer_t *o) {
    if (o->pos > 0) {
        write(o->fd, o->buf, o->pos);
        o->pos = 0;
    }
}

static void out_write(out_writer_t *o, const char *s, size_t len) {
    while (len > 0) {
        size_t space = sizeof(o->buf) - o->pos;
        if (space == 0) { out_flush(o); space = sizeof(o->buf); }
        size_t chunk = len < space ? len : space;
        memcpy(o->buf + o->pos, s, chunk);
        o->pos += chunk;
        s += chunk;
        len -= chunk;
    }
}

static void out_str(out_writer_t *o, const char *s) {
    out_write(o, s, strlen(s));
}

static void out_fmt(out_writer_t *o, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void out_fmt(out_writer_t *o, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) out_write(o, tmp, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Section formatters                                                   */
/* ------------------------------------------------------------------ */

static void fmt_metadata(mg_reader_t *r, uint32_t payload_len,
                         const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    int64_t timestamp = read_i64(r);
    uint8_t trigger = read_u8(r);
    uint16_t ver_idx = read_u16(r);

    time_t t = (time_t)timestamp;
    struct tm tm;
    localtime_r(&t, &tm);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    const char *trig_str = "manual";
    if (trigger == MG_TRIGGER_THRESHOLD) trig_str = "threshold";
    else if (trigger == MG_TRIGGER_CRITICAL) trig_str = "critical";

    out_str(o, "=== MemoryGraph Report ===\n");
    out_fmt(o, "Version:   %s\n", strtab_get(st, ver_idx));
    out_fmt(o, "Date:      %s\n", timebuf);
    out_fmt(o, "Trigger:   %s\n", trig_str);

    r->pos = end;
}

static void fmt_device(mg_reader_t *r, uint32_t payload_len,
                       const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint16_t model_idx = read_u16(r);
    uint16_t os_idx = read_u16(r);
    uint64_t phys = read_u64(r);

    char sz[32];
    fmt_size(sz, sizeof(sz), phys);

    out_str(o, "\n--- Device ---\n");
    out_fmt(o, "Model:        %s\n", strtab_get(st, model_idx));
    out_fmt(o, "OS:           %s\n", strtab_get(st, os_idx));
    out_fmt(o, "Physical RAM: %s\n", sz);

    r->pos = end;
}

static void fmt_app(mg_reader_t *r, uint32_t payload_len, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint64_t footprint = read_u64(r);
    uint64_t available = read_u64(r);

    char sz1[32], sz2[32];
    fmt_size(sz1, sizeof(sz1), footprint);
    fmt_size(sz2, sizeof(sz2), available);

    out_str(o, "\n--- App ---\n");
    out_fmt(o, "Footprint: %s\n", sz1);
    out_fmt(o, "Available: %s\n", sz2);

    r->pos = end;
}

static void fmt_vm_summary(mg_reader_t *r, uint32_t payload_len,
                           const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint64_t total_virt = read_u64(r);
    uint64_t total_res = read_u64(r);
    uint16_t tag_count = read_u16(r);

    char sz1[32], sz2[32];
    fmt_size(sz1, sizeof(sz1), total_virt);
    fmt_size(sz2, sizeof(sz2), total_res);

    out_fmt(o, "\n--- VM Summary (%u tags) ---\n", tag_count);
    out_fmt(o, "Virtual: %s  Resident: %s\n", sz1, sz2);

    for (uint16_t i = 0; i < tag_count; i++) {
        uint16_t name_idx = read_u16(r);
        uint32_t rgn_count = read_u32(r);
        uint64_t res = read_u64(r);

        char rsz[32];
        fmt_size(rsz, sizeof(rsz), res);
        out_fmt(o, "  %-24s %4u rgn  %10s\n",
                strtab_get(st, name_idx), rgn_count, rsz);
    }

    r->pos = end;
}

static void fmt_vm_regions(mg_reader_t *r, uint32_t payload_len,
                           out_writer_t *o) {
    size_t end = r->pos + payload_len;
    uint32_t count = read_u32(r);

    out_fmt(o, "\n--- VM Regions (%u total) ---\n", count);

    /* Skip individual region listing for text — too verbose.
     * The summary above has the useful aggregation. */
    r->pos = end;
}

static void fmt_heap(mg_reader_t *r, uint32_t payload_len,
                     const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint64_t total_alloc = read_u64(r);
    uint64_t total_blocks = read_u64(r);
    uint64_t total_objects = read_u64(r);
    uint8_t zone_count = read_u8(r);

    char sz[32];
    fmt_size(sz, sizeof(sz), total_alloc);

    uint64_t raw_count = total_blocks - total_objects;

    out_str(o, "\n--- Heap ---\n");
    out_fmt(o, "Allocated: %s  Blocks: %" PRIu64 "  ObjC: %" PRIu64
            "  C/C++: %" PRIu64 "\n",
            sz, total_blocks, total_objects, raw_count);

    if (zone_count > 0) {
        out_str(o, "Zones:\n");
        for (uint8_t i = 0; i < zone_count; i++) {
            uint16_t name_idx = read_u16(r);
            uint64_t allocated = read_u64(r);
            uint64_t blocks = read_u64(r);

            char asz[32];
            fmt_size(asz, sizeof(asz), allocated);
            out_fmt(o, "  %-24s %10s  %8" PRIu64 " blocks\n",
                    strtab_get(st, name_idx), asz, blocks);
        }
    }

    uint16_t top_count = read_u16(r);
    if (top_count > 0) {
        out_str(o, "\nTop classes:\n");
        for (uint16_t i = 0; i < top_count; i++) {
            uint16_t name_idx = read_u16(r);
            uint32_t count = read_u32(r);
            uint64_t total_size = read_u64(r);

            char tsz[32];
            fmt_size(tsz, sizeof(tsz), total_size);
            out_fmt(o, "  %-30s %6u  %10s\n",
                    strtab_get(st, name_idx), count, tsz);
        }
    }

    r->pos = end;
}

static void fmt_leak(mg_reader_t *r, uint32_t payload_len,
                     const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint16_t suspect_count = read_u16(r);
    if (suspect_count == 0) { r->pos = end; return; }

    out_fmt(o, "\n--- Leak Suspects (%u) ---\n", suspect_count);

    static const char *reason_names[] = {
        "abnormal_count", "abnormal_size", "high_ratio"
    };

    for (uint16_t i = 0; i < suspect_count; i++) {
        uint16_t name_idx = read_u16(r);
        uint32_t count = read_u32(r);
        uint64_t total_size = read_u64(r);
        uint8_t reason = read_u8(r);

        const char *reason_str = reason < 3 ? reason_names[reason] : "unknown";
        char sz[32];
        fmt_size(sz, sizeof(sz), total_size);
        out_fmt(o, "  %-24s %6u inst  %10s  [%s]\n",
                strtab_get(st, name_idx), count, sz, reason_str);
    }

    r->pos = end;
}

static void fmt_raw_alloc(mg_reader_t *r, uint32_t payload_len,
                          out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint64_t raw_total_size = read_u64(r);
    uint32_t raw_count      = read_u32(r);

    uint32_t buckets[6];
    for (int i = 0; i < 6; i++)
        buckets[i] = read_u32(r);

    char sz[32];
    fmt_size(sz, sizeof(sz), raw_total_size);
    out_fmt(o, "\n--- C/C++ Allocations ---\n");
    out_fmt(o, "Count: %" PRIu32 "   Total: %s\n", raw_count, sz);

    static const char *bucket_labels[] = {
        "[16B, 256B) ",
        "[256B, 1KB) ",
        "[1KB, 4KB)  ",
        "[4KB, 16KB) ",
        "[16KB, 256KB)",
        "[256KB+)     ",
    };

    out_str(o, "Size distribution:\n");
    for (int i = 0; i < 6; i++) {
        out_fmt(o, "  %s  %8" PRIu32 "\n", bucket_labels[i], buckets[i]);
    }

    r->pos = end;
}

static void fmt_reachability(mg_reader_t *r, uint32_t payload_len,
                             const read_string_table_t *st, out_writer_t *o) {
    size_t end = r->pos + payload_len;

    uint32_t total_reachable   = read_u32(r);
    uint32_t total_unreachable = read_u32(r);
    uint32_t root_count        = read_u32(r);
    uint16_t suspect_count     = read_u16(r);

    if (suspect_count == 0) { r->pos = end; return; }

    uint64_t total_size = 0;
    /* Pre-scan to compute total size. */
    size_t save_pos = r->pos;
    for (uint16_t i = 0; i < suspect_count; i++) {
        read_u16(r); read_u32(r);
        total_size += read_u64(r);
    }
    r->pos = save_pos;

    char sz[32];
    fmt_size(sz, sizeof(sz), total_size);
    out_fmt(o, "\n--- Unreachable Objects (%u classes, %u objects, %s) ---\n",
            suspect_count, total_unreachable, sz);
    out_fmt(o, "  [roots: %u, reachable: %u, unreachable: %u]\n",
            root_count, total_reachable, total_unreachable);

    for (uint16_t i = 0; i < suspect_count; i++) {
        uint16_t name_idx = read_u16(r);
        uint32_t count    = read_u32(r);
        uint64_t size     = read_u64(r);

        char s[32];
        fmt_size(s, sizeof(s), size);
        out_fmt(o, "  %-24s %6u inst  %10s\n",
                strtab_get(st, name_idx), count, s);
    }

    r->pos = end;
}

/* Edge type names for v2+ reports. */
static const char *edge_type_label(uint8_t t) {
    switch (t) {
    case 0: return NULL;           /* MG_EDGE_IVAR — use ivar_name directly */
    case 1: return "block";        /* MG_EDGE_BLOCK */
    case 2: return "collection";   /* MG_EDGE_COLLECTION */
    case 3: return "associated";   /* MG_EDGE_ASSOCIATED */
    default: return NULL;          /* MG_EDGE_UNKNOWN */
    }
}

static void fmt_ref_graph(mg_reader_t *r, uint32_t payload_len,
                          const read_string_table_t *st, out_writer_t *o,
                          uint16_t version) {
    size_t end = r->pos + payload_len;

    uint32_t total_nodes = read_u32(r);
    uint32_t total_edges = read_u32(r);
    uint32_t obj_dropped = read_u32(r);
    uint32_t edge_dropped = read_u32(r);
    uint32_t cyc_drop_len = read_u32(r);
    uint32_t cyc_drop_lim = read_u32(r);
    uint32_t sys_filtered = read_u32(r);
    uint16_t cycle_count = read_u16(r);

    out_fmt(o, "\n--- Retain Cycles (%u found) ---\n", cycle_count);
    out_fmt(o, "Graph: %u nodes, %u edges\n", total_nodes, total_edges);

    if (obj_dropped || edge_dropped || cyc_drop_len || cyc_drop_lim) {
        out_fmt(o, "Truncation: %u obj, %u edges, %u cycles(len), "
                "%u cycles(limit) dropped\n",
                obj_dropped, edge_dropped, cyc_drop_len, cyc_drop_lim);
    }
    if (sys_filtered) {
        out_fmt(o, "System cycles filtered: %u\n", sys_filtered);
    }

    for (uint16_t i = 0; i < cycle_count; i++) {
        uint8_t length = read_u8(r);
        uint32_t inst_count = read_u32(r);
        uint64_t total_size = read_u64(r);
        uint8_t is_app = read_u8(r);

        char sz[32];
        fmt_size(sz, sizeof(sz), total_size);
        out_fmt(o, "\n#%u [%u nodes, %u inst, %s, %s]\n",
                i + 1, length, inst_count, sz,
                is_app ? "APP" : "SYSTEM");

        for (uint8_t j = 0; j < length; j++) {
            uint16_t from_idx = read_u16(r);

            /* v2+: edge_type byte between from_class and ivar_name. */
            uint8_t edge_type = 4; /* MG_EDGE_UNKNOWN default for v1 */
            if (version >= 2)
                edge_type = read_u8(r);

            uint16_t ivar_idx = read_u16(r);
            uint16_t to_idx = read_u16(r);

            const char *label = edge_type_label(edge_type);
            if (label) {
                out_fmt(o, "   %s -[%s](%s)-> %s\n",
                        strtab_get(st, from_idx),
                        label,
                        strtab_get(st, ivar_idx),
                        strtab_get(st, to_idx));
            } else {
                out_fmt(o, "   %s -(%s)-> %s\n",
                        strtab_get(st, from_idx),
                        strtab_get(st, ivar_idx),
                        strtab_get(st, to_idx));
            }
        }
    }

    r->pos = end;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static int read_file(const char *path, uint8_t **out_data, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return MG_ERR_IO;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return MG_ERR_IO; }

    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size);
    if (!data) { close(fd); return MG_ERR_ALLOC; }

    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, data + total, size - total);
        if (n <= 0) { free(data); close(fd); return MG_ERR_IO; }
        total += (size_t)n;
    }
    close(fd);

    *out_data = data;
    *out_size = size;
    return MG_OK;
}

static int parse_header(mg_reader_t *r, uint16_t *out_version,
                        uint16_t *flags, uint32_t *section_count) {
    if (!reader_has(r, 16)) return MG_ERR_IO;

    uint32_t magic = read_u32(r);
    if (magic != MG_BINARY_MAGIC) return MG_ERR_IO;

    uint16_t version = read_u16(r);
    if (version > MG_BINARY_VERSION) return MG_ERR_IO;

    *out_version = version;
    *flags = read_u16(r);
    *section_count = read_u32(r);
    read_u32(r);   /* reserved */

    return MG_OK;
}

int mg_report_read_text(const char *binary_path, int output_fd) {
    if (!binary_path || output_fd < 0) return MG_ERR_INVALID_ARG;

    uint8_t *data = NULL;
    size_t size = 0;
    int rc = read_file(binary_path, &data, &size);
    if (rc != MG_OK) return rc;

    mg_reader_t r = { .data = data, .size = size, .pos = 0 };

    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t section_count = 0;
    rc = parse_header(&r, &version, &flags, &section_count);
    if (rc != MG_OK) {
        free(data);
        return rc;
    }

    read_string_table_t st = { 0 };
    out_writer_t o = { .fd = output_fd, .pos = 0 };

    for (uint32_t s = 0; s < section_count; s++) {
        if (!reader_has(&r, 5)) break;

        uint8_t type = read_u8(&r);
        uint32_t payload_len = read_u32(&r);

        if (!reader_has(&r, payload_len)) break;

        switch ((mg_section_type_t)type) {
        case MG_SEC_STRING_TABLE:
            if (!parse_string_table(&r, payload_len, &st)) {
                free(data);
                return MG_ERR_IO;
            }
            break;
        case MG_SEC_METADATA:
            fmt_metadata(&r, payload_len, &st, &o);
            if (flags & MG_FLAG_TRUNCATED) {
                out_str(&o, "Truncated: yes\n");
            }
            break;
        case MG_SEC_DEVICE:
            fmt_device(&r, payload_len, &st, &o);
            break;
        case MG_SEC_APP:
            fmt_app(&r, payload_len, &o);
            break;
        case MG_SEC_VM_SUMMARY:
            fmt_vm_summary(&r, payload_len, &st, &o);
            break;
        case MG_SEC_VM_REGIONS:
            fmt_vm_regions(&r, payload_len, &o);
            break;
        case MG_SEC_HEAP:
            fmt_heap(&r, payload_len, &st, &o);
            break;
        case MG_SEC_LEAK:
            fmt_leak(&r, payload_len, &st, &o);
            break;
        case MG_SEC_REF_GRAPH:
            fmt_ref_graph(&r, payload_len, &st, &o, version);
            break;
        case MG_SEC_RAW_ALLOC:
            fmt_raw_alloc(&r, payload_len, &o);
            break;
        case MG_SEC_REACHABILITY:
            fmt_reachability(&r, payload_len, &st, &o);
            break;
        default:
            /* Unknown section — skip for forward compatibility. */
            r.pos += payload_len;
            break;
        }
    }

    out_str(&o, "\n");
    out_flush(&o);

    free_string_table(&st);
    free(data);
    return MG_OK;
}

int mg_report_validate(const char *binary_path) {
    if (!binary_path) return MG_ERR_INVALID_ARG;

    uint8_t *data = NULL;
    size_t size = 0;
    int rc = read_file(binary_path, &data, &size);
    if (rc != MG_OK) return rc;

    mg_reader_t r = { .data = data, .size = size, .pos = 0 };

    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t section_count = 0;
    rc = parse_header(&r, &version, &flags, &section_count);
    if (rc != MG_OK) { free(data); return rc; }

    /* Must have at least one section (string table). */
    if (section_count == 0 || !reader_has(&r, 5)) {
        free(data);
        return MG_ERR_IO;
    }

    /* First section must be string table. */
    uint8_t first_type = read_u8(&r);
    if (first_type != MG_SEC_STRING_TABLE) {
        free(data);
        return MG_ERR_IO;
    }

    free(data);
    (void)flags;
    return MG_OK;
}
