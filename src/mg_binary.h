/*
 * mg_binary.h — Binary report format (MGRP)
 *
 * Compact binary format for OOM diagnostic reports.
 * Uses a string table for deduplication and fixed-size records
 * for minimal file size (~15x smaller than JSON).
 *
 * File layout:
 *   [Header 16B] [Section...] [Section...] ...
 *   Each section: type(1B) + payload_length(4B) + payload
 *   String table must be the first section.
 */

#ifndef MG_BINARY_H
#define MG_BINARY_H

#include "mg_pool.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Format constants                                                    */
/* ------------------------------------------------------------------ */

#define MG_BINARY_MAGIC       0x4D475250u  /* "MGRP" little-endian */
#define MG_BINARY_VERSION     2
#define MG_BINARY_BUF_SIZE    4096

/* File header flags (bitfield). */
#define MG_FLAG_TRUNCATED          (1u << 0)
#define MG_FLAG_REF_GRAPH_ENABLED  (1u << 1)

/* Section types. */
typedef enum {
    MG_SEC_STRING_TABLE = 1,
    MG_SEC_METADATA     = 2,
    MG_SEC_DEVICE       = 3,
    MG_SEC_APP          = 4,
    MG_SEC_VM_SUMMARY   = 5,
    MG_SEC_VM_REGIONS   = 6,
    MG_SEC_HEAP         = 7,
    MG_SEC_LEAK         = 8,
    MG_SEC_REF_GRAPH    = 9,
    MG_SEC_REACHABILITY = 10,
    MG_SEC_RAW_ALLOC    = 11,
} mg_section_type_t;

/* Trigger reason encoding. */
#define MG_TRIGGER_THRESHOLD  0
#define MG_TRIGGER_CRITICAL   1
#define MG_TRIGGER_MANUAL     2

/* Leak reason encoding (matches mg_leak_reason_t). */
#define MG_LEAK_REASON_ABNORMAL_COUNT  0
#define MG_LEAK_REASON_ABNORMAL_SIZE   1
#define MG_LEAK_REASON_HIGH_RATIO      2

/* ------------------------------------------------------------------ */
/* Binary writer (buffered fd, same pattern as mg_json_writer)         */
/* ------------------------------------------------------------------ */

typedef struct {
    int     fd;
    char    buf[MG_BINARY_BUF_SIZE];
    size_t  buf_pos;
    bool    error;   /* sticky error flag */
} mg_binary_writer_t;

void mg_binary_init(mg_binary_writer_t *w, int fd);
bool mg_binary_finish(mg_binary_writer_t *w);

/* Primitive writers — all little-endian. */
void mg_binary_u8(mg_binary_writer_t *w, uint8_t v);
void mg_binary_u16(mg_binary_writer_t *w, uint16_t v);
void mg_binary_u32(mg_binary_writer_t *w, uint32_t v);
void mg_binary_u64(mg_binary_writer_t *w, uint64_t v);
void mg_binary_i64(mg_binary_writer_t *w, int64_t v);
void mg_binary_bytes(mg_binary_writer_t *w, const void *data, size_t len);

/* Write a section envelope: type + payload_length. */
void mg_binary_section_header(mg_binary_writer_t *w,
                              mg_section_type_t type,
                              uint32_t payload_length);

/* ------------------------------------------------------------------ */
/* String table (pool-allocated, deduplicating)                        */
/* ------------------------------------------------------------------ */

#define MG_STRING_TABLE_INVALID 0xFFFFu

/* Hash slot for O(1) string dedup. */
typedef struct {
    uint32_t str_hash;    /* cached FNV-1a hash */
    uint16_t str_index;   /* index into strings[], 0xFFFF = empty */
} mg_strtab_slot_t;

typedef struct {
    const char       **strings;     /* pool-allocated pointer array */
    uint16_t           count;
    uint16_t           capacity;
    mg_strtab_slot_t  *hash_slots;  /* pool-allocated, power-of-2 sized */
    uint16_t           hash_mask;   /* slot_count - 1 */
} mg_string_table_t;

/*
 * Initialize a string table with room for `capacity` strings.
 * Returns false if pool allocation fails.
 */
bool mg_string_table_init(mg_string_table_t *t,
                          mg_pool_t *pool,
                          uint16_t capacity);

/*
 * Add a string to the table.  Returns the uint16 index.
 * Deduplicates: if str was already added, returns the existing index.
 * Returns MG_STRING_TABLE_INVALID if the table is full or str is NULL.
 */
uint16_t mg_string_table_add(mg_string_table_t *t, const char *str);

/*
 * Find a string in the table.  Returns the uint16 index.
 * Returns MG_STRING_TABLE_INVALID if str is not in the table.
 * O(1) amortized via hash table.
 */
uint16_t mg_string_table_find(const mg_string_table_t *t, const char *str);

/*
 * Write the string table as a binary section.
 * Must be called before any section that references string indices.
 */
void mg_binary_write_string_table(mg_binary_writer_t *w,
                                  const mg_string_table_t *t);

#endif /* MG_BINARY_H */
