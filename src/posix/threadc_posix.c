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
#include <time.h>
#include <unistd.h> /* sysconf */

#include <assert.h> /* static_assert */

/* What tc_thread_t's opaque bytes hold on this platform. */
typedef struct
{
    pthread_t handle;
    tc_thread_fn fn;
    void *arg;
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

/* pthreads calls this with the pointer we gave pthread_create; it unpacks
 * fn and arg and calls the user's function. */
static void *trampoline(void *p)
{
    posix_thread_t *pt = p;
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

    (void)name;

    posix_thread_t *pt = as_posix(t);
    pt->fn = fn;   /* fill in BEFORE pthread_create...                  */
    pt->arg = arg; /* ...it guarantees the new thread sees these values */
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
