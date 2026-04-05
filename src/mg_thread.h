/*
 * mg_thread.h — Thread suspension / resumption
 *
 * Suspends all threads except the calling thread before snapshot
 * collection, and resumes them afterward.  Uses Mach kernel APIs.
 */

#ifndef MG_THREAD_H
#define MG_THREAD_H

#include <stdint.h>
#include <stdbool.h>
#include <mach/mach.h>

/* Handle returned by mg_threads_suspend, passed to mg_threads_resume. */
typedef struct {
    thread_act_array_t threads;     /* array of suspended thread ports  */
    mach_msg_type_number_t count;   /* number of suspended threads      */
    bool timed_out;                 /* set if timeout fired mid-collect */
} mg_thread_state_t;

/*
 * Suspend all threads in the current task except the calling thread.
 * Populates `state` for later resumption.
 * Returns true on success (all threads suspended).
 */
bool mg_threads_suspend(mg_thread_state_t *state);

/*
 * Resume all previously suspended threads.
 */
void mg_threads_resume(mg_thread_state_t *state);

/*
 * Get the current thread's register state via inline assembly.
 * Stores the program counter (PC) and stack pointer (SP).
 */
void mg_thread_capture_regs(uint64_t *pc_out, uint64_t *sp_out);

#endif /* MG_THREAD_H */
