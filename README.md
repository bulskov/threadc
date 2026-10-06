# threadc

Portable threads, locks and time for C11 — pthreads and Win32 backends.

> **Status: design draft.** The interface in
> [`include/threadc/threadc.h`](include/threadc/threadc.h) is a proposal; nothing
> is implemented yet. The reasoning behind it is in
> [`docs/interface.md`](docs/interface.md).

## Intent

`threadc` is one of a small family of reusable C11 libraries:

| Library | Role |
|---|---|
| [arena_allocation](https://github.com/bulskov/arena_allocation) | memory: `allocator_t` and arenas |
| [seqc](https://github.com/bulskov/seqc) | collections, `string_t`, `iter_t` |
| [ctt](https://github.com/bulskov/ctt) | unit tests |
| **threadc** | threads, locks, time |
| [filec](https://github.com/bulskov/filec) | paths and filesystem |

It was started for fskim (a fast, handmade file manager), but has no knowledge
of it and should be useful anywhere.

Goals:

- **Small and predictable.** Threads, mutexes, condition variables, once, and a
  monotonic clock. Nothing more in the core header.
- **No allocation in the primitives.** `tc_mutex_t`, `tc_cond_t`, … are
  fixed-size storage structs you embed wherever you like.
- **Explicit ownership.** Every started thread is joined by its creator — no
  detach, same model as the arenas.
- **Don't wrap what C11 already gives you.** Atomics come from `<stdatomic.h>`,
  thread-locals from `_Thread_local`.
- **Same behaviour on every platform**, verified by the same tests in CI on
  Linux and Windows, under ThreadSanitizer.

## Planned layout

```
include/threadc/threadc.h   core: threads, mutex, cond, once, time
include/threadc/sema.h      counting semaphore      (portable, on top of core)
include/threadc/queue.h     bounded MPMC queue      (portable, on top of core)
include/threadc/pool.h      worker pool, one arena per worker
src/posix/                  pthreads backend
src/win32/                  Win32 backend
src/common/                 sema, queue, pool — no OS code
tests/                      ctt tests, run under TSan
```

## Dependencies

- `seqc` (and through it `arena_allocation`) — `string_t` in the core API, and
  collections / allocators for the queue and pool.
- `ctt` — tests only.

Fetched with CMake `FetchContent`, like `seqc` fetches `arena_allocation`. Use
the dependency names `arena`, `seqc`, `ctt` so a parent project's declarations
win and diamond dependencies resolve to one copy.

## Build

CMake ≥ 3.20 and Ninja. Dependencies are fetched at configure time.

```sh
./build.sh            # debug build      (presets: debug, release, asan, tsan, dev)
./test.sh             # build + ctest    (modes:   debug, asan, tsan, dev)
./test.sh dev         # against local checkouts ../seqc and ../ctt
```

The `asan` and `tsan` presets use clang. `test.sh` also fails if `malloc`/`free`
appear in `src/` — memory comes from the caller's `allocator_t`.

Use from another project:

```cmake
FetchContent_Declare(threadc
    GIT_REPOSITORY https://github.com/bulskov/threadc.git
    GIT_TAG        main)
FetchContent_MakeAvailable(threadc)
target_link_libraries(app PRIVATE threadc::threadc)
```

## License

MIT — see [LICENSE](LICENSE).
