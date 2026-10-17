#pragma once
/* threadc — internal: one step of a timed wait on a predicate, shared by
 * the semaphore and the queue.
 *
 *     uint64_t start = tc_time_now_ns();
 *     tc_mutex_lock(m);
 *     while (!predicate)
 *         if (!wait_step(c, m, start, timeout_ns))
 *             -> timed out (m still held)
 *
 * The predicate is checked before the time, so a wakeup that comes with
 * the timeout is not lost. */

#include "threadc/threadc.h"

/* Waits on c for what is left of timeout_ns since start.  False, without
 * waiting, once all of it has passed.  TC_FOREVER (and anything of 100
 * years or more) leaves a remainder tc_cond_wait_timeout treats as no
 * limit. */
static inline bool wait_step(
    tc_cond_t *c, tc_mutex_t *m, uint64_t start, uint64_t timeout_ns)
{
    uint64_t elapsed = tc_time_now_ns() - start;
    if (elapsed >= timeout_ns)
    {
        return false;
    }
    (void)tc_cond_wait_timeout(c, m, timeout_ns - elapsed);
    return true;
}
