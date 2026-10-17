/* threadc — bounded MPMC queue: a ring buffer guarded by one mutex, with a
 * condition variable for each way a thread can be held up.
 *
 * push waits on not_full and signals not_empty; pop the other way round.
 * One signal per element is enough: every waiter on a condition waits for
 * the same thing, and wait_step checks the predicate before the time, so a
 * waiter that is signalled as it times out still takes the element.  close
 * broadcasts both, since every waiter must see it. */

#include "threadc/queue.h"

#include "wait.h"

#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>

static unsigned char *slot(tc_queue_t *q, size_t i)
{
    return q->buf + (i % q->capacity) * q->elem_size;
}

tc_err_t tc_queue_init(
    tc_queue_t *q, size_t elem_size, size_t capacity, allocator_t a)
{
    if (!q || elem_size == 0 || capacity == 0
        || capacity > SIZE_MAX / elem_size)
    {
        return (tc_err_t){TC_INVALID, 0};
    }
    unsigned char *buf =
        mem_alloc(a, elem_size * capacity, alignof(max_align_t));
    if (!buf)
    {
        return (tc_err_t){TC_RESOURCE, 0};
    }
    *q = (tc_queue_t){
        .buf = buf,
        .elem_size = elem_size,
        .capacity = capacity,
        .alloc = a,
    };
    tc_mutex_init(&q->mutex);
    tc_cond_init(&q->not_empty);
    tc_cond_init(&q->not_full);
    return (tc_err_t){TC_OK, 0};
}

void tc_queue_destroy(tc_queue_t *q)
{
    assert(q && q->buf);
    tc_cond_destroy(&q->not_full);
    tc_cond_destroy(&q->not_empty);
    tc_mutex_destroy(&q->mutex);
    mem_free(q->alloc, q->buf, q->elem_size * q->capacity);
    q->buf = NULL;
}

tc_err_kind_t tc_queue_push_timeout(
    tc_queue_t *q, const void *elem, uint64_t timeout_ns)
{
    assert(q && q->buf && elem);
    uint64_t start = tc_time_now_ns();
    tc_mutex_lock(&q->mutex);
    while (q->len == q->capacity && !q->closed)
    {
        if (!wait_step(&q->not_full, &q->mutex, start, timeout_ns))
        {
            tc_mutex_unlock(&q->mutex);
            return TC_TIMEOUT;
        }
    }
    if (q->closed)
    {
        tc_mutex_unlock(&q->mutex);
        return TC_CLOSED;
    }
    memcpy(slot(q, q->head + q->len), elem, q->elem_size);
    q->len++;
    tc_mutex_unlock(&q->mutex);
    tc_cond_signal(&q->not_empty);
    return TC_OK;
}

tc_err_kind_t tc_queue_pop_timeout(
    tc_queue_t *q, void *out, uint64_t timeout_ns)
{
    assert(q && q->buf && out);
    uint64_t start = tc_time_now_ns();
    tc_mutex_lock(&q->mutex);
    while (q->len == 0 && !q->closed)
    {
        if (!wait_step(&q->not_empty, &q->mutex, start, timeout_ns))
        {
            tc_mutex_unlock(&q->mutex);
            return TC_TIMEOUT;
        }
    }
    if (q->len == 0) /* closed and drained */
    {
        tc_mutex_unlock(&q->mutex);
        return TC_CLOSED;
    }
    memcpy(out, slot(q, q->head), q->elem_size);
    q->head = (q->head + 1) % q->capacity;
    q->len--;
    tc_mutex_unlock(&q->mutex);
    tc_cond_signal(&q->not_full);
    return TC_OK;
}

bool tc_queue_push(tc_queue_t *q, const void *elem)
{
    return tc_queue_push_timeout(q, elem, TC_FOREVER) == TC_OK;
}

bool tc_queue_pop(tc_queue_t *q, void *out)
{
    return tc_queue_pop_timeout(q, out, TC_FOREVER) == TC_OK;
}

void tc_queue_close(tc_queue_t *q)
{
    assert(q && q->buf);
    tc_mutex_lock(&q->mutex);
    q->closed = true;
    tc_mutex_unlock(&q->mutex);
    tc_cond_broadcast(&q->not_empty);
    tc_cond_broadcast(&q->not_full);
}

size_t tc_queue_len(tc_queue_t *q)
{
    assert(q && q->buf);
    tc_mutex_lock(&q->mutex);
    size_t len = q->len;
    tc_mutex_unlock(&q->mutex);
    return len;
}
