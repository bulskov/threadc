/* threadc — tc_once, the same code on every platform: C11 atomics plus
 * tc_thread_yield, no OS calls.
 *
 * Written by hand instead of wrapping pthread_once / InitOnceExecuteOnce so
 * that an all-zero tc_once_t (TC_ONCE_INIT, or static storage) is valid
 * everywhere: macOS's PTHREAD_ONCE_INIT is not all zeros. */

#include "threadc/threadc.h"

#include <assert.h>
#include <stdatomic.h>

enum
{
    ONCE_NOT_STARTED = 0, /* the all-zero state */
    ONCE_RUNNING = 1,
    ONCE_DONE = 2,
};

_Static_assert(
    sizeof(atomic_int) <= sizeof(tc_once_t),
    "tc_once_t is too small for atomic_int");
_Static_assert(
    _Alignof(atomic_int) <= _Alignof(tc_once_t),
    "tc_once_t is under-aligned for atomic_int");

static atomic_int *as_state(tc_once_t *o)
{
    return (atomic_int *)o->opaque;
}

void tc_once(tc_once_t *o, void (*fn)(void))
{
    assert(o != NULL && fn != NULL);
    atomic_int *state = as_state(o);

    /* Fast path, taken by every call after the first.  The acquire pairs
     * with the release store below: whatever fn wrote is visible here. */
    if (atomic_load_explicit(state, memory_order_acquire) == ONCE_DONE)
        return;

    /* Exactly one thread moves NOT_STARTED -> RUNNING and runs fn. */
    int expected = ONCE_NOT_STARTED;
    if (atomic_compare_exchange_strong_explicit(
            state,
            &expected,
            ONCE_RUNNING,
            memory_order_acquire,
            memory_order_acquire))
    {
        fn();
        atomic_store_explicit(state, ONCE_DONE, memory_order_release);
        return;
    }

    /* Another thread is running fn: wait for it to finish.  Initialisation
     * functions are short, so yielding is enough.  (fn calling tc_once on
     * the same tc_once_t would wait here forever — documented in the
     * header.) */
    while (atomic_load_explicit(state, memory_order_acquire) != ONCE_DONE)
        tc_thread_yield();
}
