/*
 * memory_graph.h — MemoryGraph SDK Public API
 *
 * iOS OOM diagnostics: captures memory snapshots when the app
 * is about to be killed by Jetsam, producing a binary report (.mgbin).
 *
 * Language: C11
 * Min deployment: iOS 13.0
 */

#ifndef MEMORY_GRAPH_H
#define MEMORY_GRAPH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Export macro ---------- */

#if defined(__GNUC__) || defined(__clang__)
#define MG_EXPORT __attribute__((visibility("default")))
#else
#define MG_EXPORT
#endif

/* ---------- Version ---------- */

#ifndef MEMORY_GRAPH_VERSION
#define MEMORY_GRAPH_VERSION "1.0.0"
#endif

/* ---------- Error codes ---------- */

#define MG_OK               0
#define MG_ERR_ALREADY_INIT -1
#define MG_ERR_NOT_INIT     -2
#define MG_ERR_ALLOC        -3
#define MG_ERR_IO           -4
#define MG_ERR_INVALID_ARG  -5

/* ---------- Configuration ---------- */

typedef struct {
    uint64_t memory_threshold;       /* trigger threshold (bytes), default 100 MB  */
    uint32_t polling_interval_ms;    /* poll interval (ms), default 1000           */
    uint32_t suspension_timeout_ms;  /* thread suspension timeout (ms), default 30000 */
    uint64_t pool_size;              /* pre-allocated pool size (bytes), default 50 MB */
    uint32_t top_n_classes;          /* number of top classes to report, default 50 */
    bool     enable_reference_graph; /* enable reference graph analysis, default false */
    bool     enable_assoc_tracking;  /* track objc_setAssociatedObject for cycle detection, default false */
    bool     filter_system_cycles;   /* skip retain cycles of only system classes, default true */
    bool     enable_reachability_leak; /* reachability-based leak detection, default false    */
    const char *output_dir;          /* output directory (NULL = default sandbox path) */
} mg_config_t;

/*
 * Returns a config struct filled with sensible defaults.
 */
MG_EXPORT mg_config_t mg_config_default(void);

/* ---------- Callback ---------- */

/*
 * Called whenever a report is generated (manual trigger, OOM detection,
 * or on next launch when a pending report from a previous run is found).
 * report_path: null-terminated path to the binary report file (.mgbin).
 */
typedef void (*mg_report_callback_t)(const char *report_path);

/* ---------- Lifecycle ---------- */

/*
 * Initialize the SDK with the given configuration.
 * Pre-allocates the memory pool.  Must be called before mg_start().
 * Returns MG_OK on success, or a negative error code.
 */
MG_EXPORT int mg_init(const mg_config_t *config);

/*
 * Register a callback for report delivery on next launch.
 */
MG_EXPORT void mg_set_callback(mg_report_callback_t cb);

/*
 * Start memory pressure monitoring.
 * Begins the two-phase detection:
 *   Phase 1: dispatch source listens for MEMORYPRESSURE_WARN
 *   Phase 2: high-frequency polling of os_proc_available_memory()
 */
MG_EXPORT void mg_start(void);

/*
 * Stop monitoring and cancel any pending polling.
 */
MG_EXPORT void mg_stop(void);

/*
 * Release all resources (memory pool, dispatch sources).
 * After this call, mg_init() may be called again.
 */
MG_EXPORT void mg_destroy(void);

/* ---------- Manual trigger ---------- */

/*
 * Immediately trigger a memory snapshot collection.
 * Useful for debugging and testing.
 * Suspends all other threads, collects data, writes binary report, resumes threads.
 */
MG_EXPORT void mg_trigger_now(void);

/* ---------- Pending report (next launch) ---------- */

/*
 * Returns true if a report from a previous run exists on disk.
 */
MG_EXPORT bool mg_has_pending_report(void);

/*
 * Returns the file path of the pending report, or NULL.
 * The string is valid until mg_clear_pending_report() is called.
 */
MG_EXPORT const char *mg_pending_report_path(void);

/*
 * Delete the pending report file from disk.
 */
MG_EXPORT void mg_clear_pending_report(void);

/* ---------- Report conversion ---------- */

/*
 * Convert a binary report (.mgbin) to human-readable text.
 * Writes the text to `output_path` (created/truncated).
 * Returns MG_OK on success, or a negative error code.
 */
MG_EXPORT int mg_report_to_text(const char *report_path, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_GRAPH_H */
