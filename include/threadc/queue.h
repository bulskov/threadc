#pragma once
/* threadc — bounded multi-producer, multi-consumer queue of fixed-size
 * elements.  Portable: a mutex and two condition variables over a ring
 * buffer, no OS code of its own.
 *
 * Elements are copied in and out (elem_size bytes each), in FIFO order.
 * The ring buffer is the only allocation: it comes from the caller's
 * allocator in tc_queue_init and goes back in tc_queue_destroy, both on
 * the owner's thread, so the allocator need not be thread-safe.
 *
 * Shutdown is tc_queue_close: it wakes every waiting thread, after which
 * push fails at once and pop returns what is left, then fails.  Producers
 * and consumers see TC_CLOSED (or false) and stop; the owner joins them and
 * destroys the queue.
 *
 * The struct is visible so it can be embedded anywhere; its fields are
 * private. */

#include "arena/allocator.h"
#include "threadc/threadc.h"

typedef struct
{
    tc_mutex_t mutex;
    tc_cond_t not_empty;
    tc_cond_t not_full;
    unsigned char *buf;
    size_t elem_size;
    size_t capacity; /* in elements                 */
    size_t head;     /* index of the oldest element */
    size_t len;
    bool closed;
    allocator_t alloc;
} tc_queue_t;

/* Room for capacity elements of elem_size bytes each, from a.
 * TC_INVALID for a NULL q, a zero elem_size or capacity, or a size that
 * overflows; TC_RESOURCE when a has no room.  On failure q is left
 * unusable and needs no destroy. */
tc_err_t tc_queue_init(
    tc_queue_t *q, size_t elem_size, size_t capacity, allocator_t a);

/* Gives the buffer back to the allocator.  No thread may be using q:
 * close it and join the producers and consumers first. */
void tc_queue_destroy(tc_queue_t *q);

/* Copies elem in, waiting while the queue is full.  False when the queue
 * is closed (also when it closes during the wait): elem was not added. */
bool tc_queue_push(tc_queue_t *q, const void *elem);

/* Copies the oldest element to out, waiting while the queue is empty.
 * False once the queue is closed and empty: out is untouched. */
bool tc_queue_pop(tc_queue_t *q, void *out);

/* As push and pop, for at most timeout_ns (relative, monotonic clock).
 * TC_OK, TC_TIMEOUT (still full / still empty), or TC_CLOSED.
 * 0 only tries; TC_FOREVER waits with no limit. */
tc_err_kind_t tc_queue_push_timeout(
    tc_queue_t *q, const void *elem, uint64_t timeout_ns);
tc_err_kind_t tc_queue_pop_timeout(
    tc_queue_t *q, void *out, uint64_t timeout_ns);

/* Closes the queue and wakes every waiting thread.  Calling it again does
 * nothing; a closed queue does not reopen. */
void tc_queue_close(tc_queue_t *q);

/* Elements in the queue right now — already stale when it returns, unless
 * no other thread uses q. */
size_t tc_queue_len(tc_queue_t *q);
