/* threadc — Win32 backend (SRWLOCK, CONDITION_VARIABLE, INIT_ONCE).
 *
 */

#define WIN32_LEAN_AND_MEAN

#define NS_PER_S UINT64_C(1000000000)

#include "threadc/threadc.h"

#include <assert.h>
#include <errno.h>
#include <process.h> /* _beginthreadex */
#include <stdint.h>
#include <windows.h>

typedef struct
{
    HANDLE handle;
    tc_thread_fn fn;
    void *arg;
} win32_thread_t;

_Static_assert(
    sizeof(win32_thread_t) <= sizeof(tc_thread_t),
    "tc_thread_t is too small for win32_thread_t");
_Static_assert(
    _Alignof(win32_thread_t) <= _Alignof(tc_thread_t),
    "tc_thread_t is under-aligned for win32_thread_t");

static win32_thread_t *as_win32(tc_thread_t *t)
{
    return (win32_thread_t *)t->opaque;
}

static unsigned __stdcall trampoline(void *p)
{
    win32_thread_t *wt = p;
    wt->fn(wt->arg);
    return 0;
}

tc_err_t tc_thread_start(
    tc_thread_t *t, tc_thread_fn fn, void *arg, string_t name)
{
    (void)name;

    if (!t || !fn)
    {
        return (tc_err_t){TC_INVALID, 0};
    }

    win32_thread_t *wt = as_win32(t);
    wt->fn = fn;
    wt->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, trampoline, wt, 0, NULL);
    if (h == 0)
    {
        int e = errno;
        if (e == EAGAIN || e == EACCES)
        {
            return (tc_err_t){TC_RESOURCE, e};
        }

        return (tc_err_t){TC_OS, e};
    }

    wt->handle = (HANDLE)h;

    return (tc_err_t){TC_OK, 0};
}

void tc_thread_join(tc_thread_t *t)
{

    win32_thread_t *wt = as_win32(t);
    DWORD rc = WaitForSingleObject(wt->handle, INFINITE);
    assert(rc == WAIT_OBJECT_0 && "tc_thread_join: failed to join thread");
    (void)rc; /* unused when NDEBUG removes the assert */
    int rc2 = CloseHandle(wt->handle);
    assert(rc2 && "tc_thread_join: failed to close thread handle");
    (void)rc2; /* unused when NDEBUG removes the assert */
    wt->handle = NULL;
}

uint64_t tc_thread_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}

void tc_thread_yield(void)
{
    SwitchToThread();
}

int tc_cpu_count(void)
{
    int n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? n : 1;
}

/* --- Mutex -------------------------------------------------------------- */

_Static_assert(
    sizeof(SRWLOCK) <= sizeof(tc_mutex_t),
    "tc_mutex_t is too small for SRWLOCK");
_Static_assert(
    _Alignof(SRWLOCK) <= _Alignof(tc_mutex_t),
    "tc_mutex_t is under-aligned for SRWLOCK");

static SRWLOCK *as_srwlock(tc_mutex_t *m)
{
    return (SRWLOCK *)m->opaque;
}

void tc_mutex_init(tc_mutex_t *m)
{
    assert(m != NULL);
    InitializeSRWLock(as_srwlock(m));
}

void tc_mutex_destroy(tc_mutex_t *m)
{
    assert(m != NULL);
    /* SRW locks do not need explicit destruction on Windows */
}

void tc_mutex_lock(tc_mutex_t *m)
{
    assert(m != NULL);
    AcquireSRWLockExclusive(as_srwlock(m));
}

bool tc_mutex_try_lock(tc_mutex_t *m)
{
    assert(m != NULL);
    return TryAcquireSRWLockExclusive(as_srwlock(m)) != 0;
}

void tc_mutex_unlock(tc_mutex_t *m)
{
    assert(m != NULL);
    ReleaseSRWLockExclusive(as_srwlock(m));
}

/* --- Condition variable ------------------------------------------------- */

_Static_assert(
    sizeof(CONDITION_VARIABLE) <= sizeof(tc_cond_t),
    "tc_cond_t is too small for CONDITION_VARIABLE");
_Static_assert(
    _Alignof(CONDITION_VARIABLE) <= _Alignof(tc_cond_t),
    "tc_cond_t is under-aligned for CONDITION_VARIABLE");

/* Timeouts at least this long (100 years) mean "no limit": the wait becomes
 * a plain wait, which also keeps now + timeout far from overflowing. */
#define FOREVER_NS (UINT64_C(100) * 365 * 24 * 3600 * NS_PER_S)

static CONDITION_VARIABLE *as_condvar(tc_cond_t *c)
{
    return (CONDITION_VARIABLE *)c->opaque;
}

void tc_cond_init(tc_cond_t *c)
{
    assert(c != NULL);
    InitializeConditionVariable(as_condvar(c));
}

void tc_cond_destroy(tc_cond_t *c)
{
    assert(c != NULL);
    /* Condition variables do not need explicit destruction on Windows */
}

void tc_cond_wait(tc_cond_t *c, tc_mutex_t *m)
{
    assert(c != NULL && m != NULL);
    BOOL ok =
        SleepConditionVariableSRW(as_condvar(c), as_srwlock(m), INFINITE, 0);
    assert(ok && "tc_cond_wait: wait failed");
    (void)ok;
}

tc_err_kind_t tc_cond_wait_timeout(
    tc_cond_t *c, tc_mutex_t *m, uint64_t timeout_ns)
{
    assert(c != NULL && m != NULL);
    if (timeout_ns >= FOREVER_NS)
    {
        tc_cond_wait(c, m);
        return TC_OK;
    }

    /* Windows waits in whole milliseconds and its timer may end a wait a
     * little early, so wait against a deadline: only report TC_TIMEOUT once
     * the full timeout has really passed.  The same loop also splits
     * timeouts too long for one DWORD of milliseconds. */
    const uint64_t ns_per_ms = UINT64_C(1000000);
    uint64_t deadline = tc_time_now_ns() + timeout_ns;
    for (;;)
    {
        uint64_t now = tc_time_now_ns();
        if (now >= deadline)
            return TC_TIMEOUT;
        uint64_t left = deadline - now;
        uint64_t ms = left / ns_per_ms + (left % ns_per_ms != 0);
        DWORD wait_ms = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;

        if (SleepConditionVariableSRW(as_condvar(c), as_srwlock(m), wait_ms, 0))
            return TC_OK;
        DWORD err = GetLastError();
        assert(err == ERROR_TIMEOUT && "tc_cond_wait_timeout: wait failed");
        (void)err;
    }
}

void tc_cond_signal(tc_cond_t *c)
{
    assert(c != NULL);
    WakeConditionVariable(as_condvar(c));
}

void tc_cond_broadcast(tc_cond_t *c)
{
    assert(c != NULL);
    WakeAllConditionVariable(as_condvar(c));
}

/* --- Time --------------------------------------------------------------- */

uint64_t tc_time_now_ns(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remaining_ticks = ticks % ticks_per_second;

    return seconds * NS_PER_S + remaining_ticks * NS_PER_S / ticks_per_second;
}

void tc_sleep_ns(uint64_t ns)
{
    const uint64_t ns_per_ms = UINT64_C(1000000);
    uint64_t milliseconds = ns / ns_per_ms + (ns % ns_per_ms != 0);

    while (milliseconds != 0)
    {
        DWORD chunk =
            milliseconds >= MAXDWORD ? MAXDWORD - 1 : (DWORD)milliseconds;
        Sleep(chunk);
        milliseconds -= chunk;
    }
}
