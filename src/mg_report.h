/*
 * mg_report.h — Binary report generation (.mgbin)
 *
 * Takes snapshots from vm_region and heap modules, writes
 * a compact binary report to a file.
 */

#ifndef MG_REPORT_H
#define MG_REPORT_H

#include "mg_pool.h"
#include "mg_vm_region.h"
#include "mg_heap.h"
#include "mg_leak.h"
#include "mg_ref_graph.h"
#include "mg_reach.h"

/* All data needed to write a report. */
typedef struct {
    const char             *trigger_reason;  /* "threshold" / "critical" / "manual" */
    mg_vm_snapshot_t       *vm_snap;
    mg_heap_snapshot_t     *heap_snap;
    mg_leak_result_t       *leak_result;
    mg_ref_graph_result_t  *ref_result;      /* NULL if reference graph disabled */
    mg_reach_result_t      *reach_result;    /* NULL if reachability disabled    */
    bool                    truncated;       /* pool ran out of memory */
} mg_report_data_t;

/*
 * Write the complete binary report (.mgbin) to `path`.
 * String table is allocated from `pool`.
 * Returns 0 on success, negative on error.
 */
int mg_report_write(const char *path, const mg_report_data_t *data,
                    mg_pool_t *pool);

#endif /* MG_REPORT_H */
