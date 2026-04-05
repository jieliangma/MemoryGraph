/*
 * mg_leak.h — Statistical memory leak analysis
 *
 * Examines the heap snapshot to flag classes with suspiciously
 * high instance counts or sizes as potential leaks.
 */

#ifndef MG_LEAK_H
#define MG_LEAK_H

#include "mg_heap.h"
#include "mg_pool.h"
#include <stdint.h>

/* Why a class is flagged as a suspect. */
typedef enum {
    MG_LEAK_ABNORMAL_COUNT,      /* instance count > threshold       */
    MG_LEAK_ABNORMAL_SIZE,       /* total size dominates heap        */
    MG_LEAK_HIGH_RATIO,          /* >50% of all ObjC objects         */
} mg_leak_reason_t;

/* One suspected leak entry. */
typedef struct {
    const char      *class_name;
    uint32_t         count;
    uint64_t         total_size;
    mg_leak_reason_t reason;
} mg_leak_suspect_t;

/* Analysis configuration. */
typedef struct {
    uint32_t count_threshold;    /* flag if count > this (default 10000) */
    double   size_ratio;         /* flag if total_size > ratio * heap    */
    double   count_ratio;        /* flag if count > ratio * total_objects */
} mg_leak_config_t;

/* Default configuration. */
mg_leak_config_t mg_leak_config_default(void);

/* Result of analysis. */
typedef struct {
    mg_leak_suspect_t *suspects;
    uint32_t           suspect_count;
} mg_leak_result_t;

/*
 * Run statistical analysis on the heap snapshot.
 * Results are allocated from `pool`.
 * Returns 0 on success.
 */
int mg_leak_analyze(mg_pool_t *pool,
                    const mg_heap_snapshot_t *snap,
                    const mg_leak_config_t *cfg,
                    mg_leak_result_t *out);

/*
 * Return a human-readable string for a leak reason.
 */
const char *mg_leak_reason_string(mg_leak_reason_t reason);

#endif /* MG_LEAK_H */
