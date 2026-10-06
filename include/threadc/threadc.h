#pragma once
/* threadc — portable threads, locks and time.  C11.
 *
 * STATUS: DRAFT INTERFACE — not implemented yet.  See docs/interface.md.
 *
 * Backends: pthreads (Linux, macOS), Win32 (SRWLOCK, CONDITION_VARIABLE).
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
/* Sized for the largest backend (pthread_mutex_t is 64 bytes on macOS).
 * Each backend static_asserts that its native type fits.  The sizes below
 * are estimates — the static_asserts decide. */

typedef struct
{
    _Alignas(8) unsigned char opaque[16];
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

/* name is copied and truncated to the OS limit (15 bytes on Linux);
 * it shows up in debuggers, perf and htop. */
tc_err_t tc_thread_start(
    tc_thread_t *t, tc_thread_fn fn, void *arg, string_t name);

/* Every started thread must be joined exactly once.  No detach: the creator
 * owns the thread's lifetime, as with arenas. */
void tc_thread_join(tc_thread_t *t);

uint64_t tc_thread_id(void); /* stable id of the calling thread */
void tc_thread_yield(void);
int tc_cpu_count(void); /* logical CPUs available to this process */

/* --- Mutex -------------------------------------------------------------- */

void tc_mutex_init(tc_mutex_t *m); /* cannot fail on any supported backend */
void tc_mutex_destroy(tc_mutex_t *m);
void tc_mutex_lock(tc_mutex_t *m);
bool tc_mutex_try_lock(tc_mutex_t *m);
void tc_mutex_unlock(tc_mutex_t *m);

/* --- Condition variable ------------------------------------------------- */

void tc_cond_init(tc_cond_t *c);
void tc_cond_destroy(tc_cond_t *c);

/* Spurious wakeups happen: always wait in a loop on your predicate. */
void tc_cond_wait(tc_cond_t *c, tc_mutex_t *m);

/* Returns TC_OK or TC_TIMEOUT.  timeout is relative, in nanoseconds. */
tc_err_kind_t tc_cond_wait_timeout(
    tc_cond_t *c, tc_mutex_t *m, uint64_t timeout_ns);

void tc_cond_signal(tc_cond_t *c);
void tc_cond_broadcast(tc_cond_t *c);

/* --- Once --------------------------------------------------------------- */

/* Runs fn exactly once across all threads; others block until it returns. */
void tc_once(tc_once_t *o, void (*fn)(void));

/* --- Time --------------------------------------------------------------- */

uint64_t tc_time_now_ns(void); /* monotonic, unrelated to wall clock */
void tc_sleep_ns(uint64_t ns);
