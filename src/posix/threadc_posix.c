/* threadc — pthreads backend (Linux; macOS).
 *
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#define NS_PER_S UINT64_C(1000000000)

#include "threadc/threadc.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h> /* sched_yield */
#include <stdint.h>
#include <string.h> /* memcpy */
#include <time.h>
#include <unistd.h> /* sysconf */

#if defined(__linux__)
#include <sys/prctl.h> /* prctl(PR_SET_NAME) */
#endif

#include <assert.h> /* static_assert */

/* Thread names: Linux allows 15 bytes plus the NUL; macOS allows more, but
 * one limit everywhere keeps names predictable. */
#define NAME_CAP 16

/* What tc_thread_t's opaque bytes hold on this platform. */
typedef struct
{
    pthread_t handle;
    tc_thread_fn fn;
    void *arg;
    char name[NAME_CAP]; /* copied at start: the caller's buffer may die */
} posix_thread_t;

/* The public struct must have room for ours: the compiler checks it. */
static_assert(
    sizeof(posix_thread_t) <= sizeof(tc_thread_t),
    "tc_thread_t is too small for posix_thread_t");
static_assert(
    _Alignof(posix_thread_t) <= _Alignof(tc_thread_t),
    "tc_thread_t is under-aligned for posix_thread_t");

static posix_thread_t *as_posix(tc_thread_t *t)
{
    return (posix_thread_t *)t->opaque;
}

/* Copy name into dst (cap bytes including the NUL), truncating at a whole
 * UTF-8 character so a multi-byte character is never cut in half. */
static void copy_name(char *dst, size_t cap, string_t name)
{
    size_t n = name.ptr ? name.len : 0;
    if (n > cap - 1)
    {
        n = cap - 1;
        /* Back off over continuation bytes (10xxxxxx) to a character
         * boundary. */
        while (n > 0 && ((unsigned char)name.ptr[n] & 0xC0) == 0x80)
        {
            n--;
        }
    }
    if (n > 0)
    {
        memcpy(dst, name.ptr, n);
    }
    dst[n] = '\0';
}

/* Name the CALLING thread — run first thing in the new thread, because
 * macOS can only name the current thread. */
static void set_current_thread_name(const char *name)
{
    if (name[0] == '\0')
    {
        return; /* keep the default (Linux: inherited from the creator) */
    }
#if defined(__linux__)
    prctl(PR_SET_NAME, name, 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#endif
}

/* pthreads calls this with the pointer we gave pthread_create; it unpacks
 * fn and arg and calls the user's function. */
static void *trampoline(void *p)
{
    posix_thread_t *pt = p;
    set_current_thread_name(pt->name);
    pt->fn(pt->arg);
    return NULL;
}

tc_err_t tc_thread_start(
    tc_thread_t *t, tc_thread_fn fn, void *arg, string_t name)
{
    if (!t || !fn)
    {
        return (tc_err_t){TC_INVALID, 0};
    }

    posix_thread_t *pt = as_posix(t);
    pt->fn = fn;   /* fill in BEFORE pthread_create...                  */
    pt->arg = arg; /* ...it guarantees the new thread sees these values */
    copy_name(pt->name, sizeof pt->name, name);
    int err = pthread_create(&pt->handle, NULL, trampoline, pt);

    if (err != 0)
    {
        if (err == EAGAIN)
        {
            return (tc_err_t){TC_RESOURCE, err};
        }
        return (tc_err_t){TC_OS, err};
    }

    return (tc_err_t){TC_OK, 0};
}

void tc_thread_join(tc_thread_t *t)
{
    int rc = pthread_join(as_posix(t)->handle, NULL);
    assert(rc == 0 && "tc_thread_join: invalid or already joined thread");
    (void)rc; /* unused when NDEBUG removes the assert */
}

uint64_t tc_thread_id(void)
{
    static _Thread_local char marker; /* one per thread */
    return (uint64_t)(uintptr_t)&marker;
}

void tc_thread_yield(void)
{
    sched_yield();
}

int tc_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* --- Mutex -------------------------------------------------------------- */

_Static_assert(
    sizeof(pthread_mutex_t) <= sizeof(tc_mutex_t),
    "tc_mutex_t is too small for pthread_mutex_t");
_Static_assert(
    _Alignof(pthread_mutex_t) <= _Alignof(tc_mutex_t),
    "tc_mutex_t is under-aligned for pthread_mutex_t");

static pthread_mutex_t *as_pmutex(tc_mutex_t *m)
{
    return (pthread_mutex_t *)m->opaque;
}

void tc_mutex_init(tc_mutex_t *m)
{
    assert(m != NULL);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#ifndef NDEBUG
    /* Debug: unlocking a mutex you don't own, or locking one you already
     * hold, returns an error instead of being undefined behaviour. */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#endif
    int rc = pthread_mutex_init(as_pmutex(m), &attr);
    pthread_mutexattr_destroy(&attr);
    assert(rc == 0);
    (void)rc;
}

void tc_mutex_destroy(tc_mutex_t *m)
{
    assert(m != NULL);
    int rc = pthread_mutex_destroy(as_pmutex(m));
    assert(rc == 0);
    (void)rc;
}

void tc_mutex_lock(tc_mutex_t *m)
{
    assert(m != NULL);
    int rc = pthread_mutex_lock(as_pmutex(m));
    assert(rc == 0);
    (void)rc;
}

bool tc_mutex_try_lock(tc_mutex_t *m)
{
    assert(m != NULL);
    int rc = pthread_mutex_trylock(as_pmutex(m));
    assert(rc == 0 || rc == EBUSY);
    return rc == 0;
}

void tc_mutex_unlock(tc_mutex_t *m)
{
    assert(m != NULL);
    int rc = pthread_mutex_unlock(as_pmutex(m));
    assert(rc == 0);
    (void)rc;
}

/* --- Condition variable ------------------------------------------------- */

_Static_assert(
    sizeof(pthread_cond_t) <= sizeof(tc_cond_t),
    "tc_cond_t is too small for pthread_cond_t");
_Static_assert(
    _Alignof(pthread_cond_t) <= _Alignof(tc_cond_t),
    "tc_cond_t is under-aligned for pthread_cond_t");

/* Timeouts at least this long (100 years) mean "no limit": the wait becomes
 * a plain wait, which also keeps now + timeout far from overflowing. */
#define FOREVER_NS (UINT64_C(100) * 365 * 24 * 3600 * NS_PER_S)

static pthread_cond_t *as_pcond(tc_cond_t *c)
{
    return (pthread_cond_t *)c->opaque;
}

void tc_cond_init(tc_cond_t *c)
{
    assert(c != NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__)
    /* Time timed waits on the monotonic clock, so a wall-clock jump (NTP, a
     * user changing the time) neither stretches nor cuts them short.  macOS
     * has no setclock; it waits on a relative time instead (see below). */
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    int rc = pthread_cond_init(as_pcond(c), &attr);
    pthread_condattr_destroy(&attr);
    assert(rc == 0);
    (void)rc;
}

void tc_cond_destroy(tc_cond_t *c)
{
    assert(c != NULL);
    int rc = pthread_cond_destroy(as_pcond(c));
    assert(rc == 0 && "tc_cond_destroy: threads are still waiting");
    (void)rc;
}

void tc_cond_wait(tc_cond_t *c, tc_mutex_t *m)
{
    assert(c != NULL && m != NULL);
    int rc = pthread_cond_wait(as_pcond(c), as_pmutex(m));
    assert(rc == 0 && "tc_cond_wait: mutex not held by this thread?");
    (void)rc;
}

tc_err_kind_t tc_cond_wait_timeout(
    tc_cond_t *c, tc_mutex_t *m, uint64_t timeout_ns)
{
    assert(c != NULL && m != NULL);
    if (timeout_ns == 0)
    {
        return TC_TIMEOUT;
    }
    if (timeout_ns >= FOREVER_NS)
    {
        tc_cond_wait(c, m);
        return TC_OK;
    }

#if defined(__APPLE__)
    struct timespec rel = {
        .tv_sec = (time_t)(timeout_ns / NS_PER_S),
        .tv_nsec = (long)(timeout_ns % NS_PER_S),
    };
    int rc =
        pthread_cond_timedwait_relative_np(as_pcond(c), as_pmutex(m), &rel);
#else
    /* pthread_cond_timedwait takes an ABSOLUTE deadline on the clock chosen
     * in tc_cond_init — the same CLOCK_MONOTONIC as tc_time_now_ns. */
    uint64_t deadline = tc_time_now_ns() + timeout_ns;
    struct timespec abs = {
        .tv_sec = (time_t)(deadline / NS_PER_S),
        .tv_nsec = (long)(deadline % NS_PER_S),
    };
    int rc = pthread_cond_timedwait(as_pcond(c), as_pmutex(m), &abs);
#endif
    assert(
        (rc == 0 || rc == ETIMEDOUT)
        && "tc_cond_wait_timeout: mutex not held by this thread?");
    return rc == ETIMEDOUT ? TC_TIMEOUT : TC_OK;
}

void tc_cond_signal(tc_cond_t *c)
{
    assert(c != NULL);
    int rc = pthread_cond_signal(as_pcond(c));
    assert(rc == 0);
    (void)rc;
}

void tc_cond_broadcast(tc_cond_t *c)
{
    assert(c != NULL);
    int rc = pthread_cond_broadcast(as_pcond(c));
    assert(rc == 0);
    (void)rc;
}

/* --- Time --------------------------------------------------------------- */

uint64_t tc_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

void tc_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long)(ns % NS_PER_S);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}
