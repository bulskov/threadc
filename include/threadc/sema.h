#pragma once
/* threadc — counting semaphore.  Portable: a mutex, a condition variable
 * and a count, no OS code of its own (macOS has no unnamed POSIX
 * semaphores).
 *
 * The struct is visible so it can be embedded anywhere without an
 * allocator; its fields are private. */

#include "threadc/threadc.h"

typedef struct
{
    tc_mutex_t mutex;
    tc_cond_t cond;
    uint64_t count;
} tc_sema_t;

/* Starts with count units.  Cannot fail. */
void tc_sema_init(tc_sema_t *s, uint64_t count);

/* No thread may be waiting. */
void tc_sema_destroy(tc_sema_t *s);

/* Takes one unit, waiting while there are none. */
void tc_sema_wait(tc_sema_t *s);

/* As tc_sema_wait, for at most timeout_ns (relative, monotonic clock):
 * TC_OK when a unit was taken, TC_TIMEOUT when none came in time.
 * 0 only tries; TC_FOREVER waits with no limit. */
tc_err_kind_t tc_sema_wait_timeout(tc_sema_t *s, uint64_t timeout_ns);

/* Gives back one unit and wakes one waiter, if any. */
void tc_sema_post(tc_sema_t *s);
