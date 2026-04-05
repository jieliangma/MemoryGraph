/*
 * mg_core.c — SDK lifecycle and snapshot orchestration
 *
 * Glues together: pool, trigger, thread, vm_region, heap, report.
 *
 * Flow:
 *   mg_init()   → allocate pool, create trigger
 *   mg_start()  → start monitoring
 *   [trigger fires or mg_trigger_now()]
 *     1. build class set (threads still running)
 *     2. suspend all other threads
 *     3. collect VM regions
 *     4. collect heap objects
 *     5. resume threads
 *     6. write binary report (.mgbin)
 *   mg_stop()   → stop monitoring
 *   mg_destroy()→ release pool, trigger
 */

#include "../include/memory_graph.h"
#include "mg_pool.h"
#include "mg_trigger.h"
#include "mg_thread.h"
#include "mg_vm_region.h"
#include "mg_heap.h"
#include "mg_leak.h"
#include "mg_report.h"
#include "mg_ref_graph.h"
#include "mg_ivar_map.h"
#include "mg_reach.h"
#include "mg_assoc_tracker.h"
#include "mg_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct {
    mg_config_t            config;
    mg_pool_t             *pool;
    mg_trigger_t          *trigger;
    mg_assoc_tracker_t    *assoc_tracker;
    mg_report_callback_t   callback;
    atomic_bool            initialized;
    atomic_bool            collecting;    /* guard against re-entrant collect */
    char                   report_dir[512];
    char                   pending_path[512];
} g_mg;

/* Protects non-atomic fields of g_mg (config, pool, trigger, callback,
 * report_dir, pending_path) against concurrent access. */
static pthread_mutex_t g_mg_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

#define MG_DEFAULT_THRESHOLD      (100ULL * 1024 * 1024)  /* 100 MB */
#define MG_DEFAULT_POLL_MS        1000
#define MG_DEFAULT_TIMEOUT_MS     30000
#define MG_DEFAULT_POOL_SIZE      (50ULL * 1024 * 1024)   /* 50 MB  */
#define MG_DEFAULT_TOP_N          50

/* Reference graph sizing: one object slot per N bytes of pool.
 * Bounded to fit within the 18-bit edge target field (max 262143). */
#define MG_REF_GRAPH_BYTES_PER_OBJ  256
#define MG_REF_GRAPH_MAX_OBJS       200000
#define MG_REF_GRAPH_MIN_OBJS       1000

mg_config_t mg_config_default(void) {
    mg_config_t c;
    memset(&c, 0, sizeof(c));
    c.memory_threshold      = MG_DEFAULT_THRESHOLD;
    c.polling_interval_ms   = MG_DEFAULT_POLL_MS;
    c.suspension_timeout_ms = MG_DEFAULT_TIMEOUT_MS;
    c.pool_size             = MG_DEFAULT_POOL_SIZE;
    c.top_n_classes         = MG_DEFAULT_TOP_N;
    c.enable_reference_graph  = true;
    c.enable_assoc_tracking   = true;
    c.filter_system_cycles    = true;
    c.enable_reachability_leak = true;
    c.output_dir             = NULL;
    return c;
}

/* ------------------------------------------------------------------ */
/* Output directory                                                    */
/* ------------------------------------------------------------------ */

static void resolve_output_dir(const char *user_dir) {
    if (user_dir) {
        snprintf(g_mg.report_dir, sizeof(g_mg.report_dir), "%s", user_dir);
    } else {
        /* Default: $TMPDIR/memorygraph/
         * Strip trailing '/' from TMPDIR — iOS sets it to e.g.
         * ".../Data/tmp/" which would produce a double-slash. */
        const char *tmp = getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        size_t len = strlen(tmp);
        while (len > 1 && tmp[len - 1] == '/') len--;
        snprintf(g_mg.report_dir, sizeof(g_mg.report_dir),
                 "%.*s/memorygraph", (int)len, tmp);
    }
    mkdir(g_mg.report_dir, 0755);
}

static void make_report_path(char *buf, size_t buflen) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, buflen,
             "%s/oom_%04d%02d%02d_%02d%02d%02d.mgbin",
             g_mg.report_dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ------------------------------------------------------------------ */
/* Snapshot collection (the core routine)                               */
/* ------------------------------------------------------------------ */

static void do_collect(const char *reason) {
    /* Prevent re-entrant collection. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_mg.collecting, &expected, true)) {
        return;
    }

    mg_pool_t *pool = g_mg.pool;
    mg_pool_reset(pool);

    mg_vm_snapshot_t vm_snap;
    memset(&vm_snap, 0, sizeof(vm_snap));

    mg_heap_snapshot_t heap_snap;
    memset(&heap_snap, 0, sizeof(heap_snap));

#ifdef MG_APP_STORE_COMPLIANT
    /* Compliant mode: no thread suspension, limited enumeration. */
    mg_vm_region_collect_compat(pool, &vm_snap);
    mg_heap_collect_compat(pool, g_mg.config.top_n_classes, &heap_snap);
#else
    /* --- Phase A: build class set (threads still running) --- */
    mg_class_set_t *class_set = mg_class_set_build(pool);

    /* --- Phase A2: create reference graph builder if enabled --- */
    mg_ref_graph_t *ref_graph = NULL;
    mg_ivar_map_t  *ivar_map  = NULL;
    if (g_mg.config.enable_reference_graph) {
        uint32_t max_objs = (uint32_t)(g_mg.config.pool_size / MG_REF_GRAPH_BYTES_PER_OBJ);
        if (max_objs > MG_REF_GRAPH_MAX_OBJS) max_objs = MG_REF_GRAPH_MAX_OBJS;
        if (max_objs < MG_REF_GRAPH_MIN_OBJS) max_objs = MG_REF_GRAPH_MIN_OBJS;
        ref_graph = mg_ref_graph_create(pool, max_objs);

        /* Build ivar map for cycle annotation (uses ObjC runtime, safe here). */
        ivar_map = mg_ivar_map_build(pool);
    }

    /* --- Phase A3: enumerate heap (threads still running) ---
     * Zone introspect enumerators may acquire internal zone locks.
     * Running this BEFORE thread suspension avoids deadlock when
     * a suspended thread holds such a lock (common with libpas
     * zones on iOS 15+ / macOS 12+). */
    if (class_set) {
        mg_heap_collect(pool, class_set,
                        g_mg.config.top_n_classes, &heap_snap,
                        ref_graph);
    }

    /* --- Phase B: suspend all other threads --- */
    mg_thread_state_t thread_state;
    bool suspended = mg_threads_suspend(&thread_state);

    /* --- Phase C: collect VM regions (threads are frozen) --- */
    mg_vm_region_collect(pool, &vm_snap);

    /* --- Phase C2: scan inter-object references (threads still frozen) --- */
    if (ref_graph) {
        mg_ref_graph_build_index(ref_graph);
        mg_ref_graph_scan_references(ref_graph, heap_snap.buffer_table);

        /* Inject associated-object virtual edges (snapshot is safe
         * because other threads are suspended). */
        if (g_mg.assoc_tracker) {
            uint32_t assoc_count = 0;
            const mg_assoc_entry_t *assoc_entries =
                mg_assoc_tracker_snapshot(g_mg.assoc_tracker, &assoc_count);
            if (assoc_count > 0) {
                mg_ref_graph_inject_associations(
                    ref_graph, assoc_entries, assoc_count);
            }
        }
    }

    /* --- Phase C3: scan GC roots for reachability analysis --- */
    uint8_t *root_bits = NULL;
    uint32_t reach_root_count = 0;
    if (ref_graph && g_mg.config.enable_reachability_leak) {
        mg_reach_scan_roots(pool, ref_graph, &vm_snap,
                            &thread_state, &root_bits, &reach_root_count);
    }
    (void)reach_root_count;

    /* --- Phase D: resume threads --- */
    if (suspended) {
        mg_threads_resume(&thread_state);
    }
#endif

    /* --- Phase E: leak analysis (threads are running again) --- */
    mg_leak_result_t leak_result;
    memset(&leak_result, 0, sizeof(leak_result));
    mg_leak_config_t leak_cfg = mg_leak_config_default();
    mg_leak_analyze(pool, &heap_snap, &leak_cfg, &leak_result);

    /* --- Phase E2: reference graph cycle detection --- */
    mg_ref_graph_result_t ref_result;
    memset(&ref_result, 0, sizeof(ref_result));
#ifndef MG_APP_STORE_COMPLIANT
    if (ref_graph) {
        mg_ref_graph_options_t ref_opts = {
            .filter_system_cycles = g_mg.config.filter_system_cycles,
        };
        mg_ref_graph_find_cycles(pool, ref_graph, ivar_map,
                                 &ref_opts, &ref_result);
    }
#endif

    /* --- Phase E3: reachability-based leak detection --- */
    mg_reach_result_t reach_result;
    memset(&reach_result, 0, sizeof(reach_result));
#ifndef MG_APP_STORE_COMPLIANT
    if (root_bits) {
        mg_reach_analyze(pool, ref_graph, root_bits, &reach_result);
    }
#endif

    /* --- Phase F: write report --- */
    char path[512];
    make_report_path(path, sizeof(path));

    mg_report_data_t report = {
        .trigger_reason = reason,
        .vm_snap        = &vm_snap,
        .heap_snap      = &heap_snap,
        .leak_result    = &leak_result,
        .ref_result     = &ref_result,
        .reach_result   = reach_result.suspect_count > 0 ? &reach_result : NULL,
        .truncated      = mg_pool_truncated(pool),
    };

    int ret = mg_report_write(path, &report, pool);

    pthread_mutex_lock(&g_mg_lock);
    if (ret == 0) {
        snprintf(g_mg.pending_path, sizeof(g_mg.pending_path), "%s", path);
    }
    mg_report_callback_t cb = g_mg.callback;
    pthread_mutex_unlock(&g_mg_lock);

    atomic_store(&g_mg.collecting, false);

    /* Notify via callback after releasing both the lock and the
     * collecting guard, so re-entrant calls are not blocked. */
    if (ret == 0 && cb) {
        cb(path);
    }
}

/* Called by the trigger system when memory drops below threshold. */
static void trigger_fire_handler(void *ctx) {
    (void)ctx;
    do_collect("threshold");
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int mg_init(const mg_config_t *config) {
    /* Atomic CAS prevents TOCTOU: only one thread can win. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_mg.initialized, &expected, true)) {
        return MG_ERR_ALREADY_INIT;
    }

    pthread_mutex_lock(&g_mg_lock);

    mg_config_t cfg = config ? *config : mg_config_default();

    /* Validate. */
    if (cfg.pool_size < 1024 * 1024) cfg.pool_size = MG_DEFAULT_POOL_SIZE;
    if (cfg.top_n_classes == 0)      cfg.top_n_classes = MG_DEFAULT_TOP_N;
    if (cfg.memory_threshold == 0)   cfg.memory_threshold = MG_DEFAULT_THRESHOLD;
    if (cfg.polling_interval_ms == 0) cfg.polling_interval_ms = MG_DEFAULT_POLL_MS;
    if (cfg.suspension_timeout_ms == 0) cfg.suspension_timeout_ms = MG_DEFAULT_TIMEOUT_MS;

    /* Reachability analysis requires the reference graph. */
    if (cfg.enable_reachability_leak) cfg.enable_reference_graph = true;

    g_mg.config = cfg;

    /* Allocate pool. */
    g_mg.pool = mg_pool_create(cfg.pool_size);
    if (!g_mg.pool) {
        atomic_store(&g_mg.initialized, false);
        pthread_mutex_unlock(&g_mg_lock);
        return MG_ERR_ALLOC;
    }

    /* Resolve output directory. */
    resolve_output_dir(cfg.output_dir);

    /* Create trigger. */
    mg_trigger_config_t tcfg = {
        .memory_threshold    = cfg.memory_threshold,
        .polling_interval_ms = cfg.polling_interval_ms,
        .fire_fn             = trigger_fire_handler,
        .fire_ctx            = NULL,
    };
    g_mg.trigger = mg_trigger_create(&tcfg);
    if (!g_mg.trigger) {
        mg_pool_destroy(g_mg.pool);
        g_mg.pool = NULL;
        atomic_store(&g_mg.initialized, false);
        pthread_mutex_unlock(&g_mg_lock);
        return MG_ERR_ALLOC;
    }

    /* Install associated-object tracker if requested. */
    g_mg.assoc_tracker = NULL;
#ifndef MG_APP_STORE_COMPLIANT
    if (cfg.enable_assoc_tracking && cfg.enable_reference_graph) {
        g_mg.assoc_tracker = mg_assoc_tracker_create(4096);
    }
#endif

    g_mg.collecting = false;
    g_mg.pending_path[0] = '\0';

    pthread_mutex_unlock(&g_mg_lock);
    return MG_OK;
}

void mg_set_callback(mg_report_callback_t cb) {
    pthread_mutex_lock(&g_mg_lock);
    g_mg.callback = cb;
    pthread_mutex_unlock(&g_mg_lock);
}

void mg_start(void) {
    if (!atomic_load(&g_mg.initialized)) return;
    mg_trigger_start(g_mg.trigger);

    /* Check for pending report from previous run. */
    if (g_mg.callback && mg_has_pending_report()) {
        g_mg.callback(g_mg.pending_path);
    }
}

void mg_stop(void) {
    if (!atomic_load(&g_mg.initialized)) return;
    mg_trigger_stop(g_mg.trigger);
}

void mg_destroy(void) {
    bool expected = true;
    if (!atomic_compare_exchange_strong(&g_mg.initialized, &expected, false)) {
        return;
    }

    pthread_mutex_lock(&g_mg_lock);

    mg_trigger_destroy(g_mg.trigger);
    g_mg.trigger = NULL;

    mg_assoc_tracker_destroy(g_mg.assoc_tracker);
    g_mg.assoc_tracker = NULL;

    mg_pool_destroy(g_mg.pool);
    g_mg.pool = NULL;

    g_mg.callback = NULL;

    pthread_mutex_unlock(&g_mg_lock);
}

void mg_trigger_now(void) {
    if (!atomic_load(&g_mg.initialized)) return;
    do_collect("manual");
}

/* ------------------------------------------------------------------ */
/* Pending report                                                      */
/* ------------------------------------------------------------------ */

bool mg_has_pending_report(void) {
    pthread_mutex_lock(&g_mg_lock);
    if (g_mg.pending_path[0] == '\0') {
        pthread_mutex_unlock(&g_mg_lock);
        return false;
    }
    bool exists = access(g_mg.pending_path, F_OK) == 0;
    pthread_mutex_unlock(&g_mg_lock);
    return exists;
}

/*
 * Returns a pointer into internal storage.  Valid until the next
 * collection or mg_clear_pending_report() call.  Caller must not
 * free or modify the returned string.
 */
const char *mg_pending_report_path(void) {
    if (mg_has_pending_report()) return g_mg.pending_path;
    return NULL;
}

void mg_clear_pending_report(void) {
    pthread_mutex_lock(&g_mg_lock);
    if (g_mg.pending_path[0] != '\0') {
        unlink(g_mg.pending_path);
        g_mg.pending_path[0] = '\0';
    }
    pthread_mutex_unlock(&g_mg_lock);
}
