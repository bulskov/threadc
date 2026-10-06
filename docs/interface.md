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

- `sema.h` — counting semaphore from mutex + cond. macOS has no unnamed POSIX
  semaphores, so writing it once ourselves is simpler than three backends.
- `queue.h` — bounded MPMC queue, fixed `elem_size`, with `close()` for clean
  shutdown (wakes all waiters; push fails, pop drains then fails).
- `pool.h` — N worker threads, each owning its own arena. A job is
  `void (*)(void *arg, allocator_t scratch)`; the scratch arena is reset
  between jobs.

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

The sizes in the draft header are estimates. Known native sizes to check:

| Type | Linux x86-64 | macOS arm64 | Win32 |
|---|---|---|---|
| mutex | `pthread_mutex_t` 40 | 64 | `SRWLOCK` 8 |
| cond | `pthread_cond_t` 48 | 48 | `CONDITION_VARIABLE` 8 |
| thread | `pthread_t` 8 | 8 | `HANDLE` 8 (+ id) |
| once | `pthread_once_t` 4 | 16 | `INIT_ONCE` 8 |

*(Verify these — macOS `pthread_once_t` may force `tc_once_t` to grow.)*

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
- [ ] CI: ThreadSanitizer build (`-fsanitize=thread`; cannot be combined with
      ASan, so separate presets)

## Tests to write first

1. N threads × M increments under a mutex → exactly N·M.
2. Producer/consumer through mutex + cond, with spurious-wakeup-safe loops.
3. `tc_cond_wait_timeout` returns `TC_TIMEOUT` within a sane bound, and
   `TC_OK` when signalled before the deadline.
4. `tc_once` raced from many threads → `fn` runs exactly once.
5. Thread names are visible (Linux: `/proc/self/task/*/comm`).

## Open questions

- Should `tc_thread_start` take `string_t` (pulls in `seqc` for the core) or
  `const char *`? `string_t` is consistent with the family; `seqc` is needed
  for the queue/pool anyway.
- Does `tc_once` need a context pointer (`void (*fn)(void *)`)? pthreads does
  not support it directly; Win32 `InitOnceExecuteOnce` does.
- Windows toolchain: clang-cl only, or also MSVC? (C11 atomics in MSVC are
  still marked experimental.)
