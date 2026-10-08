/* threadc — tc_thread_id, the same code on every platform: a process-wide
 * counter, no OS calls.
 *
 * The obvious alternatives both break the promise that ids are never
 * shared:
 *   - the address of a _Thread_local variable: macOS allocates thread-local
 *     storage lazily on first use, so a thread that asks for its id after
 *     another one has exited can get that thread's freed block — the same
 *     address, the same id;
 *   - the OS thread id (gettid, GetCurrentThreadId): reused once a thread
 *     has exited.
 * A counter is never reused, and the id stays fixed once assigned. */

#include "threadc/threadc.h"

#include <stdatomic.h>

uint64_t tc_thread_id(void)
{
    static atomic_uint_fast64_t next_id = 1;
    static _Thread_local uint64_t id; /* 0: not assigned yet */
    if (id == 0)
    {
        id = (uint64_t)atomic_fetch_add_explicit(
            &next_id, 1, memory_order_relaxed);
    }
    return id;
}
