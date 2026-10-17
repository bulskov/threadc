/* threadc — worker pool: a tc_queue_t of jobs and the threads that pop it.
 *
 * Stopping is the queue's close: the workers drain what is queued, see
 * pop fail, and return; the owner joins them. */

#include "threadc/pool.h"

#include "arena/growing_arena.h"

#include <assert.h>
#include <stdalign.h>

#define DEFAULT_SCRATCH_BLOCK ((size_t)64 * 1024)

typedef struct
{
    tc_job_fn fn;
    void *arg;
} job_t;

struct tc_pool_worker
{
    tc_thread_t thread;
    growing_arena_t scratch;
    tc_pool_t *pool;
};

static void work(void *arg)
{
    tc_pool_worker_t *w = arg;
    allocator_t scratch = growing_arena_allocator(&w->scratch);
    job_t job;
    while (tc_queue_pop(&w->pool->jobs, &job))
    {
        job.fn(job.arg, scratch);
        growing_arena_reset(&w->scratch);
    }
}

/* Joins the first n workers (the queue is closed) and gives back what
 * start took. */
static void shut_down(tc_pool_t *p, int n)
{
    tc_queue_close(&p->jobs);
    for (int i = 0; i < n; ++i)
    {
        tc_thread_join(&p->workers[i].thread);
    }
    for (int i = 0; i < p->count; ++i)
    {
        growing_arena_destroy(&p->workers[i].scratch);
    }
    tc_queue_destroy(&p->jobs);
    mem_free(p->alloc, p->workers, (size_t)p->count * sizeof *p->workers);
    p->workers = NULL;
}

tc_err_t tc_pool_start(
    tc_pool_t *p,
    int workers,
    size_t queue_capacity,
    size_t scratch_block,
    allocator_t a,
    string_t name)
{
    if (!p || workers < 0 || queue_capacity == 0)
    {
        return (tc_err_t){TC_INVALID, 0};
    }
    int count = workers ? workers : tc_cpu_count();
    if (scratch_block == 0)
    {
        scratch_block = DEFAULT_SCRATCH_BLOCK;
    }

    *p = (tc_pool_t){.count = count, .alloc = a};
    p->workers = mem_alloc(
        a, (size_t)count * sizeof *p->workers, alignof(tc_pool_worker_t));
    if (!p->workers)
    {
        return (tc_err_t){TC_RESOURCE, 0};
    }
    tc_err_t e = tc_queue_init(&p->jobs, sizeof(job_t), queue_capacity, a);
    if (e.kind != TC_OK)
    {
        mem_free(a, p->workers, (size_t)count * sizeof *p->workers);
        p->workers = NULL;
        return e;
    }
    for (int i = 0; i < count; ++i)
    {
        p->workers[i].pool = p;
        growing_arena_init(&p->workers[i].scratch, scratch_block);
    }
    for (int i = 0; i < count; ++i)
    {
        e = tc_thread_start(&p->workers[i].thread, work, &p->workers[i], name);
        if (e.kind != TC_OK)
        {
            shut_down(p, i);
            return e;
        }
    }
    return (tc_err_t){TC_OK, 0};
}

void tc_pool_stop(tc_pool_t *p)
{
    assert(p && p->workers);
    shut_down(p, p->count);
}

tc_err_kind_t tc_pool_submit_timeout(
    tc_pool_t *p, tc_job_fn fn, void *arg, uint64_t timeout_ns)
{
    assert(p && p->workers && fn);
    job_t job = {fn, arg};
    return tc_queue_push_timeout(&p->jobs, &job, timeout_ns);
}

bool tc_pool_submit(tc_pool_t *p, tc_job_fn fn, void *arg)
{
    return tc_pool_submit_timeout(p, fn, arg, TC_FOREVER) == TC_OK;
}

int tc_pool_size(const tc_pool_t *p)
{
    assert(p && p->workers);
    return p->count;
}
