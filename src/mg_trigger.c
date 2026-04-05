/*
 * mg_trigger.c — Two-phase OOM trigger detection
 *
 * Phase 1 (passive): A dispatch source watches for
 *   DISPATCH_MEMORYPRESSURE_WARN.  Zero CPU cost.
 *
 * Phase 2 (active): A dispatch timer polls
 *   mg_available_memory() at the configured interval.
 *   When available memory < threshold → fire callback.
 *   When pressure returns to NORMAL → back to Phase 1.
 */

#include "mg_trigger.h"
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <TargetConditionals.h>

#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
#include <os/proc.h>
#endif

/*
 * Portable wrapper for mg_available_memory().
 * On macOS (used for testing), falls back to estimating available
 * memory from task_info and hw.memsize.
 */
static size_t mg_available_memory(void) {
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
    return os_proc_available_memory();
#else
    /* macOS fallback: physical memory - current footprint. */
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(),
                                TASK_VM_INFO,
                                (task_info_t)&vm_info,
                                &count);
    if (kr != KERN_SUCCESS) return SIZE_MAX;

    uint64_t physical = 0;
    size_t len = sizeof(physical);
    sysctlbyname("hw.memsize", &physical, &len, NULL, 0);

    int64_t avail = (int64_t)physical - (int64_t)vm_info.phys_footprint;
    return avail > 0 ? (size_t)avail : 0;
#endif
}

struct mg_trigger {
    mg_trigger_config_t  cfg;
    dispatch_source_t    pressure_source;
    dispatch_source_t    poll_timer;
    dispatch_queue_t     queue;
    atomic_int           phase;         /* 0=stopped 1=passive 2=polling */
    atomic_bool          fired;         /* prevents re-entrant fire      */
};

/* ------------------------------------------------------------------ */
/* Phase transitions                                                   */
/* ------------------------------------------------------------------ */

static void enter_phase2(mg_trigger_t *trig);
static void exit_phase2(mg_trigger_t *trig);

static void poll_handler(mg_trigger_t *trig) {
    size_t avail = mg_available_memory();

    if (avail <= trig->cfg.memory_threshold) {
        /* Prevent re-entrant firing. */
        bool expected = false;
        if (atomic_compare_exchange_strong(&trig->fired, &expected, true)) {
            if (trig->cfg.fire_fn) {
                trig->cfg.fire_fn(trig->cfg.fire_ctx);
            }
            /* After firing, stop polling — report has been written. */
            exit_phase2(trig);
        }
        return;
    }
    /* Still above threshold — keep polling. */
}

static void enter_phase2(mg_trigger_t *trig) {
    int expected = 1;
    if (!atomic_compare_exchange_strong(&trig->phase, &expected, 2)) return;

    /* Create a repeating timer on our serial queue. */
    trig->poll_timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, trig->queue);

    uint64_t interval = (uint64_t)trig->cfg.polling_interval_ms * NSEC_PER_MSEC;
    dispatch_source_set_timer(trig->poll_timer,
                              dispatch_time(DISPATCH_TIME_NOW, 0),
                              interval,
                              interval / 10);  /* 10% leeway */

    dispatch_source_set_event_handler(trig->poll_timer, ^{
        poll_handler(trig);
    });

    dispatch_resume(trig->poll_timer);
}

static void exit_phase2(mg_trigger_t *trig) {
    int expected = 2;
    if (!atomic_compare_exchange_strong(&trig->phase, &expected, 1)) return;

    if (trig->poll_timer) {
        dispatch_source_cancel(trig->poll_timer);
        trig->poll_timer = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Memory pressure handler                                             */
/* ------------------------------------------------------------------ */

static void pressure_handler(mg_trigger_t *trig) {
    unsigned long flags = dispatch_source_get_data(trig->pressure_source);

    if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
        /*
         * CRITICAL — fire immediately without waiting for poll.
         * The system may kill us any moment.
         */
        int phase = atomic_load(&trig->phase);
        if (phase >= 1) {
            bool expected = false;
            if (atomic_compare_exchange_strong(&trig->fired, &expected, true)) {
                if (trig->cfg.fire_fn) {
                    trig->cfg.fire_fn(trig->cfg.fire_ctx);
                }
            }
        }
    } else if (flags & DISPATCH_MEMORYPRESSURE_WARN) {
        /* WARN — escalate to phase 2 polling. */
        if (atomic_load(&trig->phase) == 1) {
            enter_phase2(trig);
        }
    } else if (flags & DISPATCH_MEMORYPRESSURE_NORMAL) {
        /* NORMAL — de-escalate back to phase 1. */
        if (atomic_load(&trig->phase) == 2) {
            exit_phase2(trig);
        }
        atomic_store(&trig->fired, false);  /* allow future fires */
    }
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

mg_trigger_t *mg_trigger_create(const mg_trigger_config_t *cfg) {
    if (!cfg || !cfg->fire_fn) return NULL;

    mg_trigger_t *trig = (mg_trigger_t *)calloc(1, sizeof(mg_trigger_t));
    if (!trig) return NULL;

    trig->cfg   = *cfg;
    trig->phase = 0;
    trig->fired = false;

    trig->queue = dispatch_queue_create(
        "com.memorygraph.trigger", DISPATCH_QUEUE_SERIAL);

    return trig;
}

void mg_trigger_start(mg_trigger_t *trig) {
    if (!trig) return;

    int expected = 0;
    if (!atomic_compare_exchange_strong(&trig->phase, &expected, 1)) return;

    /* Phase 1: memory pressure dispatch source. */
    trig->pressure_source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,
        0,
        DISPATCH_MEMORYPRESSURE_WARN |
        DISPATCH_MEMORYPRESSURE_CRITICAL |
        DISPATCH_MEMORYPRESSURE_NORMAL,
        trig->queue);

    dispatch_source_set_event_handler(trig->pressure_source, ^{
        pressure_handler(trig);
    });

    dispatch_resume(trig->pressure_source);
}

void mg_trigger_stop(mg_trigger_t *trig) {
    if (!trig) return;

    /* Stop polling if active. */
    if (atomic_load(&trig->phase) == 2) {
        exit_phase2(trig);
    }

    /* Cancel pressure source. */
    if (trig->pressure_source) {
        dispatch_source_cancel(trig->pressure_source);
        trig->pressure_source = NULL;
    }

    atomic_store(&trig->phase, 0);
}

void mg_trigger_destroy(mg_trigger_t *trig) {
    if (!trig) return;
    mg_trigger_stop(trig);
    free(trig);
}

int mg_trigger_phase(const mg_trigger_t *trig) {
    return trig ? atomic_load(&trig->phase) : 0;
}
