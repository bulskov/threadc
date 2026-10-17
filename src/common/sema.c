/* threadc — counting semaphore: a count guarded by a mutex, and a condition
 * variable for the threads waiting for it to become non-zero. */

#include "threadc/sema.h"

#include "wait.h"

#include <assert.h>

void tc_sema_init(tc_sema_t *s, uint64_t count)
{
    assert(s);
    tc_mutex_init(&s->mutex);
    tc_cond_init(&s->cond);
    s->count = count;
}

void tc_sema_destroy(tc_sema_t *s)
{
    assert(s);
    tc_cond_destroy(&s->cond);
    tc_mutex_destroy(&s->mutex);
}

void tc_sema_wait(tc_sema_t *s)
{
    assert(s);
    tc_mutex_lock(&s->mutex);
    while (s->count == 0)
    {
        tc_cond_wait(&s->cond, &s->mutex);
    }
    s->count--;
    tc_mutex_unlock(&s->mutex);
}

tc_err_kind_t tc_sema_wait_timeout(tc_sema_t *s, uint64_t timeout_ns)
{
    assert(s);
    uint64_t start = tc_time_now_ns();
    tc_mutex_lock(&s->mutex);
    while (s->count == 0)
    {
        if (!wait_step(&s->cond, &s->mutex, start, timeout_ns))
        {
            tc_mutex_unlock(&s->mutex);
            return TC_TIMEOUT;
        }
    }
    s->count--;
    tc_mutex_unlock(&s->mutex);
    return TC_OK;
}

void tc_sema_post(tc_sema_t *s)
{
    assert(s);
    tc_mutex_lock(&s->mutex);
    assert(s->count < UINT64_MAX && "tc_sema_post: count overflows");
    s->count++;
    tc_mutex_unlock(&s->mutex);
    tc_cond_signal(&s->cond);
}
