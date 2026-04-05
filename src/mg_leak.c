/*
 * mg_leak.c — Statistical memory leak analysis
 *
 * Scans the heap snapshot's top classes and flags those that
 * exceed configurable thresholds for instance count, total size
 * ratio, or object count ratio.
 */

#include "mg_leak.h"
#include "../include/memory_graph.h"
#include <string.h>

#define MG_LEAK_DEFAULT_COUNT_THRESHOLD  10000  /* > 10K instances of one class  */
#define MG_LEAK_DEFAULT_SIZE_RATIO      0.30   /* > 30% of total heap           */
#define MG_LEAK_DEFAULT_COUNT_RATIO     0.50   /* > 50% of all ObjC objects     */

mg_leak_config_t mg_leak_config_default(void) {
    mg_leak_config_t c;
    c.count_threshold = MG_LEAK_DEFAULT_COUNT_THRESHOLD;
    c.size_ratio      = MG_LEAK_DEFAULT_SIZE_RATIO;
    c.count_ratio     = MG_LEAK_DEFAULT_COUNT_RATIO;
    return c;
}

const char *mg_leak_reason_string(mg_leak_reason_t reason) {
    switch (reason) {
    case MG_LEAK_ABNORMAL_COUNT: return "abnormal_count";
    case MG_LEAK_ABNORMAL_SIZE:  return "abnormal_size";
    case MG_LEAK_HIGH_RATIO:     return "high_ratio";
    default:                     return "unknown";
    }
}

int mg_leak_analyze(mg_pool_t *pool,
                    const mg_heap_snapshot_t *snap,
                    const mg_leak_config_t *cfg,
                    mg_leak_result_t *out) {
    if (!pool || !snap || !out) return MG_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    mg_leak_config_t lc = cfg ? *cfg : mg_leak_config_default();

    if (snap->top_count == 0 || !snap->top_classes) return 0;

    /* Worst case: every top class is a suspect. */
    mg_leak_suspect_t *suspects = (mg_leak_suspect_t *)mg_pool_alloc(
        pool, sizeof(mg_leak_suspect_t) * snap->top_count);
    if (!suspects) return MG_ERR_ALLOC;

    uint32_t found = 0;

    for (uint32_t i = 0; i < snap->top_count; i++) {
        const mg_class_stat_t *cs = &snap->top_classes[i];
        mg_leak_reason_t reason = MG_LEAK_ABNORMAL_COUNT;
        bool is_suspect = false;

        /* Check 1: abnormally high instance count. */
        if (cs->count > lc.count_threshold) {
            reason = MG_LEAK_ABNORMAL_COUNT;
            is_suspect = true;
        }
        /* Check 2: dominates total heap size. */
        else if (snap->total_allocated > 0 &&
                 (double)cs->total_size / (double)snap->total_allocated > lc.size_ratio) {
            reason = MG_LEAK_ABNORMAL_SIZE;
            is_suspect = true;
        }
        /* Check 3: dominates total ObjC object count. */
        else if (snap->total_objects > 0 &&
                 (double)cs->count / (double)snap->total_objects > lc.count_ratio) {
            reason = MG_LEAK_HIGH_RATIO;
            is_suspect = true;
        }

        if (is_suspect) {
            suspects[found].class_name = cs->class_name;
            suspects[found].count      = cs->count;
            suspects[found].total_size = cs->total_size;
            suspects[found].reason     = reason;
            found++;
        }
    }

    out->suspects      = suspects;
    out->suspect_count = found;
    return 0;
}
