#pragma once
/* threadc — portable threads, locks and time.  C11.
 *
 * Backends: pthreads (Linux, macOS), Win32 (SRWLOCK, CONDITION_VARIABLE).
 * Design notes: docs/interface.md.
 * Not wrapped on purpose — use C11 directly:
 *   atomics       <stdatomic.h>
 *   thread-local  _Thread_local
 *
 * Misuse (unlocking a mutex you don't hold, destroying a locked mutex) is a
 * bug, not an error: debug builds assert, release builds are undefined.
 * Errors are reserved for things a correct program must handle. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seqc/string.h"

/* --- Errors ------------------------------------------------------------- */

typedef enum
{
    TC_OK = 0,
    TC_TIMEOUT,  /* timed wait expired                          */
    TC_RESOURCE, /* OS refused: too many threads, out of memory */
    TC_INVALID,  /* NULL or otherwise invalid argument          */
    TC_OS,       /* anything else; see os_code                  */
} tc_err_kind_t;

typedef struct
{
    tc_err_kind_t kind;
    int32_t os_code; /* errno / GetLastError, 0 if not from the OS */
} tc_err_t;

/* --- Opaque fixed-size storage ------------------------------------------ */
/* Each backend keeps its native object in these bytes and _Static_asserts
 * that it fits: pthread_mutex_t is 64 bytes on macOS, pthread_cond_t 48.
 * Changing a size changes the ABI. */

typedef struct
{
    _Alignas(8) unsigned char opaque[64];
} tc_thread_t;

typedef struct
{
    _Alignas(8) unsigned char opaque[64];
} tc_mutex_t;

typedef struct
{
    _Alignas(8) unsigned char opaque[48];
} tc_cond_t;

typedef struct
{
    _Alignas(8) unsigned char opaque[8];
} tc_once_t;

#define TC_ONCE_INIT {{0}}

/* --- Threads ------------------------------------------------------------ */

typedef void (*tc_thread_fn)(void *arg);

/* Starts fn(arg) on a new thread.  Returns TC_INVALID for a NULL t or fn,
 * TC_RESOURCE when the OS has no room for another thread.
 *
 * name is copied (the caller's buffer may go away) and shows up in
 * debuggers, perf, htop and Task Manager: at most 15 bytes on every
 * platform, cut at a whole UTF-8 character.  An empty name keeps the
 * platform default.
 *
 * The new thread keeps a pointer into *t: do not move or copy the
 * tc_thread_t between start and join. */
tc_err_t tc_thread_start(
    tc_thread_t *t, tc_thread_fn fn, void *arg, string_t name);

/* Every started thread must be joined exactly once.  No detach: the creator
 * owns the thread's lifetime, as with arenas. */
void tc_thread_join(tc_thread_t *t);

/* Id of the calling thread: never 0, stable for its lifetime, and never
 * reused within the process — not even after a thread has finished. */
uint64_t tc_thread_id(void);
void tc_thread_yield(void);
int tc_cpu_count(void); /* logical CPUs online; at least 1 */

/* --- Mutex -------------------------------------------------------------- */

void tc_mutex_init(tc_mutex_t *m); /* cannot fail on any supported backend */
void tc_mutex_destroy(tc_mutex_t *m);
void tc_mutex_lock(tc_mutex_t *m);
bool tc_mutex_try_lock(tc_mutex_t *m);
void tc_mutex_unlock(tc_mutex_t *m);

/* --- Condition variable ------------------------------------------------- */

void tc_cond_init(tc_cond_t *c);
void tc_cond_destroy(tc_cond_t *c);

/* Call with m held; m is released while waiting and held again on return.
 * Spurious wakeups happen: always wait in a loop on your predicate. */
void tc_cond_wait(tc_cond_t *c, tc_mutex_t *m);

/* As tc_cond_wait, for at most timeout_ns (relative, monotonic clock).
 * Returns TC_TIMEOUT once the full timeout has passed, TC_OK when woken
 * before that — spuriously too, so keep looping on the predicate with the
 * time that is left.  0 times out at once; 100 years or more waits with no
 * limit. */
tc_err_kind_t tc_cond_wait_timeout(
    tc_cond_t *c, tc_mutex_t *m, uint64_t timeout_ns);

void tc_cond_signal(tc_cond_t *c);
void tc_cond_broadcast(tc_cond_t *c);

/* --- Once --------------------------------------------------------------- */

/* Runs fn exactly once per tc_once_t, however many threads call at once;
 * every call returns only after fn has finished, so what fn initialised is
 * ready.  Valid when all zeros: TC_ONCE_INIT, static storage or memset.
 * fn must not call tc_once on the same tc_once_t (it would wait forever). */
void tc_once(tc_once_t *o, void (*fn)(void));

/* --- Time --------------------------------------------------------------- */

uint64_t tc_time_now_ns(void); /* monotonic, unrelated to wall clock */
void tc_sleep_ns(uint64_t ns); /* at least ns; signals do not cut it short */
