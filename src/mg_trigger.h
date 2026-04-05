/*
 * mg_trigger.h — Two-phase OOM trigger detection
 *
 * Phase 1: dispatch source listens for MEMORYPRESSURE_WARN (zero CPU)
 * Phase 2: high-frequency polling of os_proc_available_memory()
 *
 * When available memory drops below the configured threshold,
 * invokes the user-supplied collection callback.
 */

#ifndef MG_TRIGGER_H
#define MG_TRIGGER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Callback invoked when the trigger fires (memory below threshold).
 * Must perform the actual snapshot collection.
 */
typedef void (*mg_trigger_fire_fn)(void *ctx);

typedef struct {
    uint64_t          memory_threshold;      /* bytes */
    uint32_t          polling_interval_ms;   /* ms    */
    mg_trigger_fire_fn fire_fn;
    void             *fire_ctx;
} mg_trigger_config_t;

/* Opaque handle. */
typedef struct mg_trigger mg_trigger_t;

/*
 * Create the trigger system.  Does NOT start monitoring yet.
 * Returns NULL on failure.
 */
mg_trigger_t *mg_trigger_create(const mg_trigger_config_t *cfg);

/*
 * Start monitoring: installs the memory pressure dispatch source.
 */
void mg_trigger_start(mg_trigger_t *trig);

/*
 * Stop monitoring: cancels dispatch source and polling timer.
 */
void mg_trigger_stop(mg_trigger_t *trig);

/*
 * Destroy the trigger and free resources.
 */
void mg_trigger_destroy(mg_trigger_t *trig);

/*
 * Returns the current phase (1 = passive, 2 = polling, 0 = stopped).
 */
int mg_trigger_phase(const mg_trigger_t *trig);

#endif /* MG_TRIGGER_H */
