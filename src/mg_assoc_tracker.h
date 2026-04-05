/*
 * mg_assoc_tracker.h — Track objc_setAssociatedObject for retain cycle detection
 *
 * Hooks objc_setAssociatedObject/objc_removeAssociatedObjects at runtime
 * via Mach-O symbol rebinding.  Records (source, value) pairs for
 * OBJC_ASSOCIATION_RETAIN and OBJC_ASSOCIATION_COPY policies.
 *
 * During snapshot, the tracker provides an iterator over live associations
 * so the reference graph can inject virtual edges.
 *
 * Disabled under MG_APP_STORE_COMPLIANT (symbol rebinding uses private
 * Mach-O introspection).
 */

#ifndef MG_ASSOC_TRACKER_H
#define MG_ASSOC_TRACKER_H

#include <stdint.h>
#include <stdbool.h>

/* One recorded retain-policy association. */
typedef struct mg_assoc_entry {
    uintptr_t object;   /* source object */
    uintptr_t value;    /* associated value */
} mg_assoc_entry_t;

/* Opaque tracker handle. */
typedef struct mg_assoc_tracker mg_assoc_tracker_t;

/*
 * Create and install the tracker.
 * Hooks objc_setAssociatedObject and objc_removeAssociatedObjects.
 * Returns NULL if allocation fails or hooks cannot be installed.
 *
 * initial_capacity: starting capacity for the recording table.
 */
mg_assoc_tracker_t *mg_assoc_tracker_create(uint32_t initial_capacity);

/*
 * Destroy the tracker and unhook.
 */
void mg_assoc_tracker_destroy(mg_assoc_tracker_t *t);

/*
 * Snapshot the current associations.
 * Returns a pointer to the internal entry array and sets *count.
 *
 * MUST be called while other threads are suspended (reads the table
 * without locking).  The returned pointer is valid until the next
 * mg_assoc_tracker_destroy() call.
 */
const mg_assoc_entry_t *mg_assoc_tracker_snapshot(
    const mg_assoc_tracker_t *t,
    uint32_t *count);

#endif /* MG_ASSOC_TRACKER_H */
