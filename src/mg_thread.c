/*
 * mg_thread.c — Thread suspension / resumption via Mach APIs
 *
 * Suspends every thread in the current task except the caller,
 * collects data while they're frozen, then resumes them.
 */

#include "mg_thread.h"
#include <mach/mach.h>
#include <pthread.h>
#include <string.h>

bool mg_threads_suspend(mg_thread_state_t *state) {
    if (!state) return false;
    memset(state, 0, sizeof(*state));

    mach_port_t task = mach_task_self();
    thread_act_t self = mach_thread_self();

    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;

    kern_return_t kr = task_threads(task, &threads, &count);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(task, self);
        return false;
    }

    /*
     * Suspend every thread except ourselves.
     * We walk the list, suspending as we go.  If any single suspend
     * fails we still continue — partial suspension is better than none.
     */
    mach_msg_type_number_t suspended = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (threads[i] == self) continue;

        kr = thread_suspend(threads[i]);
        if (kr == KERN_SUCCESS) {
            /* Pack suspended threads toward the front of the array. */
            threads[suspended++] = threads[i];
        } else {
            mach_port_deallocate(task, threads[i]);
        }
    }

    /* Deallocate our own thread port (not stored in the array). */
    mach_port_deallocate(task, self);

    state->threads = threads;
    state->count   = suspended;
    return true;
}

void mg_threads_resume(mg_thread_state_t *state) {
    if (!state || !state->threads) return;

    mach_port_t task = mach_task_self();

    for (mach_msg_type_number_t i = 0; i < state->count; i++) {
        thread_resume(state->threads[i]);
        mach_port_deallocate(task, state->threads[i]);
    }

    /* Free the thread array allocated by task_threads(). */
    vm_deallocate(task,
                  (vm_address_t)state->threads,
                  sizeof(thread_act_t) * state->count);

    state->threads = NULL;
    state->count   = 0;
}

/*
 * Inline assembly to snapshot the calling thread's PC and SP.
 * Only arm64 is implemented; x86_64 is provided for simulator builds.
 */
void mg_thread_capture_regs(uint64_t *pc_out, uint64_t *sp_out) {
#if defined(__aarch64__)
    uint64_t pc, sp;
    __asm__ __volatile__(
        "adr %0, .\n"    /* PC = address of this instruction */
        "mov %1, sp\n"   /* SP = current stack pointer       */
        : "=r"(pc), "=r"(sp)
    );
    if (pc_out) *pc_out = pc;
    if (sp_out) *sp_out = sp;
#elif defined(__x86_64__)
    uint64_t pc, sp;
    __asm__ __volatile__(
        "leaq (%%rip), %0\n"
        "mov %%rsp, %1\n"
        : "=r"(pc), "=r"(sp)
    );
    if (pc_out) *pc_out = pc;
    if (sp_out) *sp_out = sp;
#else
    if (pc_out) *pc_out = 0;
    if (sp_out) *sp_out = 0;
#endif
}
