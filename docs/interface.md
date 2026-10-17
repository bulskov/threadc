# threadc — interface design (draft)

This document records *why* the interface looks the way it does, and which
questions are still open. It is a draft: change it as the implementation
teaches us things.

## Scope

**In the core header (`threadc.h`):**

- threads: start, join, id, yield, CPU count
- mutex, condition variable, once
- monotonic time and sleep (needed by timed waits anyway, so it lives here
  rather than in a separate `timec`)

**Later, in their own headers, built portably on top of the core — no
backend code:**

- `sema.h` — counting semaphore from mutex + cond (done). macOS has no unnamed POSIX
  semaphores, so writing it once ourselves is simpler than three backends.
- `queue.h` — bounded MPMC queue, fixed `elem_size`, with `close()` for clean
  shutdown (wakes all waiters; push fails, pop drains then fails) (done).
- `pool.h` — N worker threads, each owning its own arena. A job is
  `void (*)(void *arg, allocator_t scratch)`; the scratch arena is reset
  between jobs (done).

**Explicitly out of scope:**

- atomics — use `<stdatomic.h>`
- thread-local storage — use `_Thread_local`
- `detach` — see below
- recursive mutexes, read/write locks — add only if a real need shows up

## Conventions

| Convention | Reason |
|---|---|
| `tc_` prefix on everything | Unprefixed `thread_t` clashes with Mach on macOS; `thrd_*` / `mtx_*` belong to C11 `<threads.h>`. |
| `_t` suffix on types, Allman braces, 80 columns | Same style as `seqc` / `arena_allocation`. |
| Library-specific errors: `tc_err_t { kind, os_code }` | Each library in the family owns its error codes; `os_code` keeps the raw `errno` / `GetLastError()` for diagnostics. |
| Misuse asserts, does not return errors | `lock`/`unlock`/`destroy` failing is always a bug. Returning an error nobody checks is worse than a debug assert. |

## Fixed-size opaque storage instead of allocated handles

Primitives are embedded in other structs (a queue contains a mutex and two
condition variables), so they must not need an allocator:

```c
typedef struct { _Alignas(8) unsigned char opaque[64]; } tc_mutex_t;
```

Each backend casts the storage to its native type and checks the fit at
compile time:

```c
static_assert(sizeof(pthread_mutex_t) <= sizeof(tc_mutex_t), "tc_mutex_t too small");
static_assert(_Alignof(pthread_mutex_t) <= 8, "tc_mutex_t underaligned");
```

Native sizes, as the `_Static_assert`s in the backends now check them:

| Type | Linux x86-64 | macOS arm64 | Win32 |
|---|---|---|---|
| mutex | `pthread_mutex_t` 40 | 64 | `SRWLOCK` 8 |
| cond | `pthread_cond_t` 48 | 48 | `CONDITION_VARIABLE` 8 |
| thread | `pthread_t` 8 + fn, arg, name[16] = 40 | 40 | `HANDLE` 8 + fn, arg, name[16] = 40 |
| once | — (portable `atomic_int`, 4) | 4 | 4 |

`tc_thread_t` is 64 bytes, leaving room; `tc_cond_t` (48) is exactly full on
both POSIX platforms.

## Once without pthread_once

`tc_once` is the same code on every platform (`src/common/once.c`): an
`atomic_int` that one thread moves from *not started* to *running* with a
compare-and-swap, runs `fn`, and stores *done* with release ordering; other
callers yield until they see *done* with acquire ordering. Wrapping
`pthread_once` was the obvious alternative, but macOS's `PTHREAD_ONCE_INIT` is
not all zeros, and an all-zero `TC_ONCE_INIT` (or static storage) must be valid
everywhere.

## Condition variable timeouts

- **Linux:** the condition variable is created with
  `pthread_condattr_setclock(CLOCK_MONOTONIC)`, and the relative timeout becomes
  an absolute deadline on that clock — the same clock as `tc_time_now_ns`.
- **macOS:** has no `setclock`; `pthread_cond_timedwait_relative_np` takes the
  relative timeout directly.
- **Windows:** `SleepConditionVariableSRW` waits in whole milliseconds and may
  end a little early, so it runs in a deadline loop and only reports
  `TC_TIMEOUT` once the full timeout has passed.
- A timeout of 0 times out at once; 100 years or more means no limit (which
  also keeps `now + timeout` from overflowing).

## Thread ids

`tc_thread_id` is a process-wide counter, assigned on a thread's first call
and kept in a `_Thread_local` (`src/common/thread_id.c`): never 0, never
reused. Both obvious alternatives repeat ids:

- the address of a `_Thread_local` variable — glibc reuses a finished
  thread's memory, and macOS allocates thread-local storage lazily on first
  use, so even two threads alive at the same time could get one address;
- the OS thread id (`gettid`, `GetCurrentThreadId`) — reused after exit.

## Thread names

Copied into `tc_thread_t` at start and set by the trampoline in the new thread
before `fn` runs — macOS can only name the current thread. Linux
`prctl(PR_SET_NAME)`, macOS `pthread_setname_np`, Windows
`SetThreadDescription` (UTF-16). At most 15 bytes everywhere (Linux's limit),
cut at a whole UTF-8 character, so a name looks the same on every platform.

## No detach

Every started thread is joined exactly once by whoever started it. This
mirrors the arena model — the creator owns the lifetime — and makes shutdown
deterministic: when the owner returns, its threads are gone. A detached
"fire and forget" thread is replaced by a pool job.

## Time

`tc_time_now_ns()` is monotonic (`CLOCK_MONOTONIC` / `QueryPerformanceCounter`)
and only meaningful as a difference. Wall-clock time for display belongs
elsewhere (filec's timestamps are Unix epoch nanoseconds).

`tc_cond_wait_timeout` takes a *relative* timeout. Backends convert:
pthreads needs an absolute `CLOCK_MONOTONIC` deadline (`pthread_condattr_setclock`
— not available on macOS, which needs `pthread_cond_timedwait_relative_np`),
Win32 takes milliseconds.

## Semaphore and queue

Both are portable code in `src/common/`, built from the core primitives, with
their structs visible so they can be embedded without an allocator (the
fields are private).

- **Timeouts** follow `tc_cond_wait_timeout`: relative, monotonic. `0` only
  tries, and `TC_FOREVER` (`UINT64_MAX`) waits with no limit, so there is one
  timed function per operation instead of separate `try_` variants. A timed
  wait loops on its predicate with what is left of the timeout, and checks
  the predicate before the time, so a wakeup that arrives with the timeout is
  not lost (`src/common/wait.h`).
- **Queue memory:** the ring buffer is the only allocation. It comes from the
  caller's `allocator_t` in `tc_queue_init` and goes back in
  `tc_queue_destroy`, both on the owner's thread, so the allocator need not
  be thread-safe, and push/pop never allocate. Elements are fixed-size and
  copied in and out.
- **Shutdown is `tc_queue_close`**, not a sentinel element: it wakes every
  waiter; push then fails at once (`TC_CLOSED`) and pop returns what is left,
  then fails. Producers and consumers stop, the owner joins them and destroys
  the queue — the same "creator owns the lifetime" rule as threads.
- **One mutex, two condition variables** (`not_empty`, `not_full`). Every
  waiter on one of them waits for the same thing, so a push or pop signals one
  waiter; close broadcasts both. Lock-free designs were not worth it for a
  file manager's job queue; a lock-free SPSC ring may still go into `seqc`.
- `TC_CLOSED` was added to `tc_err_kind_t`, after `TC_OS`, so the existing
  values do not change.

## Pool

A `tc_queue_t` of jobs (function + argument) and N threads popping it.

- **Scratch:** each worker owns a `growing_arena_t`, reset after every job,
  so a job allocates without locks and without freeing. A growing arena
  rather than a fixed one, because a job's need is not known up front (a
  directory of 100 000 entries); `scratch_block` is only the step it grows
  by. The arenas take OS pages directly; what the pool takes from the
  caller's allocator (the workers' bookkeeping and the queue) is allocated
  in start and given back in stop, on the owner's thread.
- **Results outlive scratch**, so they go where the job's argument says: an
  arena the job fills and hands to the main thread (fskim's directory
  jobs), a queue, a slot in an array.
- **Stop drains:** it closes the queue, the workers run what is left and
  return, the owner joins them. There is no cancel and no discard — a job
  that should stop early checks a flag in its argument, which also covers a
  job that is already running.
- **Jobs may submit jobs**, but only with a timeout of 0: if every worker
  waited for room at once, nobody would make room.
- **No `wait_idle`:** the owner learns that work is done from the work
  itself (fskim: the job wakes the main loop). Add it when a real need shows
  up.

## Thread model of the library family

threadc is also the tool for testing the other libraries under threads. The
model we intend to document in `seqc` and `arena_allocation`:

1. **Collections are not synchronised** (like the C++ STL). One owner at a
   time; handing an object to another thread is fine, using it from two
   threads at once is not.
2. **Arenas are per thread.** A job gets its own arena, fills it, and hands
   the whole arena to the consumer, which destroys it in one call. No locks on
   the hot path, no per-object `free`.
3. **Explicit sharing** goes through a wrapper:
   `locked_allocator(allocator_t inner, tc_mutex_t *m)` → `allocator_t`.
   It lives in threadc (optional, depends on arena) so `arena_allocation`
   stays thread-free.
4. **Concurrent queues** live in threadc. A lock-free SPSC ring buffer may
   later go into `seqc`, since it only needs `<stdatomic.h>`.

Audit list before claiming the above:

- [ ] `seqc`: no hidden global state (initial grep found none besides
      thread-local `errno`)
- [ ] `debug_allocator_t`: counters are plain fields — wrong if shared
- [ ] `scratch_t`: must be per thread
- [ ] `ctt`: are assertions from worker threads safe? (test state and signal
      based crash isolation are probably global — start with "assert on the
      main thread only")
- [x] CI: ThreadSanitizer build (`-fsanitize=thread`; cannot be combined with
      ASan, so separate presets)

## Tests

One ctt suite per part, each built and run under ASan and TSan in CI, with a
60 s ctest timeout so a lost wakeup fails instead of hanging:

| Suite | Covers |
|---|---|
| `time_test.c` | monotonic clock, sleep (rounding, > 1 s, 0) |
| `thread_test.c` | start/join, args, reuse, invalid arguments, ids (distinct, never reused), CPU count |
| `mutex_test.c` | exclusion and visibility (8 × 50 000 increments), try_lock |
| `cond_test.c` | signal, broadcast, timeouts (relative vs absolute, 0, > 1 s, huge), producer/consumer |
| `once_test.c` | runs once, all-zero initial state, 16 racing threads |
| `name_test.c` | names read back from the OS, 15-byte cut at a UTF-8 boundary |
| `sema_test.c` | initial count, try/timeout, post wakes a waiter, every unit taken once, one unit excludes |
| `pool_test.c` | start errors, one worker per CPU, everything given back, every job once, all workers at once, full queue and stop drains, scratch reset and growth, jobs submitting jobs |
| `queue_test.c` | init errors, one allocation given back, FIFO around the ring, any element size, try/timeouts, waiting push/pop, close (drains, wakes both sides), 4 × 4 threads: every element once and in order per producer |

## Decided

- `tc_thread_start` takes `string_t`: consistent with the family, and `seqc` is
  needed for the queue and pool anyway.
- `tc_once` takes `void (*fn)(void)`, no context pointer: initialisation
  functions work on statics.
- Windows: clang-cl. MSVC's `cl` needs `/experimental:c11atomics` for
  `<stdatomic.h>`.

## Open questions

- None right now.
