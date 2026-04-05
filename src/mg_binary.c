/*
 * mg_binary.c — Binary report writer + string table
 *
 * Writes binary data to a file descriptor through a small
 * stack-resident buffer.  Never mallocs — safe during OOM collection.
 * All integers are little-endian (native on ARM64 + x86_64).
 */

#include "mg_binary.h"
#include <unistd.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Buffered writer                                                     */
/* ------------------------------------------------------------------ */

static void flush(mg_binary_writer_t *w) {
    if (w->error || w->buf_pos == 0) return;

    const char *p = w->buf;
    size_t remaining = w->buf_pos;

    while (remaining > 0) {
        ssize_t n = write(w->fd, p, remaining);
        if (n <= 0) {
            w->error = true;
            break;
        }
        p += n;
        remaining -= (size_t)n;
    }
    w->buf_pos = 0;
}

static void emit(mg_binary_writer_t *w, const void *data, size_t len) {
    if (w->error) return;

    const char *src = (const char *)data;
    while (len > 0) {
        size_t space = MG_BINARY_BUF_SIZE - w->buf_pos;
        if (space == 0) {
            flush(w);
            space = MG_BINARY_BUF_SIZE;
        }
        size_t chunk = len < space ? len : space;
        memcpy(w->buf + w->buf_pos, src, chunk);
        w->buf_pos += chunk;
        src += chunk;
        len -= chunk;
    }
}

/* ------------------------------------------------------------------ */
/* Public writer API                                                   */
/* ------------------------------------------------------------------ */

void mg_binary_init(mg_binary_writer_t *w, int fd) {
    memset(w, 0, sizeof(*w));
    w->fd = fd;
}

bool mg_binary_finish(mg_binary_writer_t *w) {
    flush(w);
    return !w->error;
}

void mg_binary_u8(mg_binary_writer_t *w, uint8_t v) {
    emit(w, &v, 1);
}

void mg_binary_u16(mg_binary_writer_t *w, uint16_t v) {
    /* ARM64 and x86_64 are both little-endian — emit directly. */
    emit(w, &v, 2);
}

void mg_binary_u32(mg_binary_writer_t *w, uint32_t v) {
    emit(w, &v, 4);
}

void mg_binary_u64(mg_binary_writer_t *w, uint64_t v) {
    emit(w, &v, 8);
}

void mg_binary_i64(mg_binary_writer_t *w, int64_t v) {
    emit(w, &v, 8);
}

void mg_binary_bytes(mg_binary_writer_t *w, const void *data, size_t len) {
    emit(w, data, len);
}

void mg_binary_section_header(mg_binary_writer_t *w,
                              mg_section_type_t type,
                              uint32_t payload_length) {
    mg_binary_u8(w, (uint8_t)type);
    mg_binary_u32(w, payload_length);
}

/* ------------------------------------------------------------------ */
/* String table (hash-based O(1) dedup)                                */
/* ------------------------------------------------------------------ */

static inline uint32_t strtab_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint32_t)(unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

bool mg_string_table_init(mg_string_table_t *t,
                          mg_pool_t *pool,
                          uint16_t capacity) {
    if (!t || !pool || capacity == 0) return false;
    memset(t, 0, sizeof(*t));

    t->strings = (const char **)mg_pool_alloc(
        pool, sizeof(const char *) * capacity);
    if (!t->strings) return false;

    /* Hash table: 2x capacity, power of 2. */
    uint16_t slot_cap = 1;
    while (slot_cap < (uint32_t)capacity * 2) slot_cap <<= 1;

    t->hash_slots = (mg_strtab_slot_t *)mg_pool_alloc(
        pool, sizeof(mg_strtab_slot_t) * slot_cap);
    if (!t->hash_slots) return false;

    /* Mark all slots empty. */
    for (uint16_t i = 0; i < slot_cap; i++) {
        t->hash_slots[i].str_index = MG_STRING_TABLE_INVALID;
    }
    t->hash_mask = slot_cap - 1;
    t->capacity = capacity;
    return true;
}

uint16_t mg_string_table_add(mg_string_table_t *t, const char *str) {
    if (!t || !str) return MG_STRING_TABLE_INVALID;

    uint32_t h = strtab_hash(str);
    uint16_t slot = (uint16_t)(h & t->hash_mask);

    /* Probe for existing entry. */
    while (t->hash_slots[slot].str_index != MG_STRING_TABLE_INVALID) {
        uint16_t idx = t->hash_slots[slot].str_index;
        if (t->hash_slots[slot].str_hash == h &&
            (t->strings[idx] == str ||
             strcmp(t->strings[idx], str) == 0)) {
            return idx;
        }
        slot = (slot + 1) & t->hash_mask;
    }

    if (t->count >= t->capacity) return MG_STRING_TABLE_INVALID;

    uint16_t idx = t->count;
    t->strings[idx] = str;
    t->count++;

    t->hash_slots[slot].str_hash  = h;
    t->hash_slots[slot].str_index = idx;
    return idx;
}

uint16_t mg_string_table_find(const mg_string_table_t *t, const char *str) {
    if (!t || !str) return MG_STRING_TABLE_INVALID;

    uint32_t h = strtab_hash(str);
    uint16_t slot = (uint16_t)(h & t->hash_mask);

    while (t->hash_slots[slot].str_index != MG_STRING_TABLE_INVALID) {
        uint16_t idx = t->hash_slots[slot].str_index;
        if (t->hash_slots[slot].str_hash == h &&
            (t->strings[idx] == str ||
             strcmp(t->strings[idx], str) == 0)) {
            return idx;
        }
        slot = (slot + 1) & t->hash_mask;
    }
    return MG_STRING_TABLE_INVALID;
}

void mg_binary_write_string_table(mg_binary_writer_t *w,
                                  const mg_string_table_t *t) {
    /* Compute payload size:
     * 2 (count) + sum of (2 + strlen) for each string. */
    uint32_t payload = 2;
    for (uint16_t i = 0; i < t->count; i++) {
        payload += 2 + (uint32_t)strlen(t->strings[i]);
    }

    mg_binary_section_header(w, MG_SEC_STRING_TABLE, payload);

    mg_binary_u16(w, t->count);
    for (uint16_t i = 0; i < t->count; i++) {
        uint16_t len = (uint16_t)strlen(t->strings[i]);
        mg_binary_u16(w, len);
        mg_binary_bytes(w, t->strings[i], len);
    }
}
