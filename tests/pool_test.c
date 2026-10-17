/* Worker pool.
 *
 *   tc_err_t      tc_pool_start(tc_pool_t *p, int workers,
 *                               size_t queue_capacity, size_t scratch_block,
 *                               allocator_t a, string_t name);
 *   void          tc_pool_stop(tc_pool_t *p);
 *   bool          tc_pool_submit(tc_pool_t *p, tc_job_fn fn, void *arg);
 *   tc_err_kind_t tc_pool_submit_timeout(p, fn, arg, timeout_ns);
 *   int           tc_pool_size(const tc_pool_t *p);
 *
 * Jobs never ASSERT: they record, the main thread checks after stop. */

#include "ctt.h"
#include "threadc/pool.h"
#include "threadc/sema.h"

#include "arena/debug_allocator.h"
#include "arena/fixed_arena.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_S UINT64_C(1000000000)

static unsigned char backing[1 << 16];
static fixed_arena_t arena;
static allocator_t heap;

void ctt_before_each(void)
{
    fixed_arena_init(&arena, backing, sizeof backing);
    heap = fixed_arena_allocator(&arena);
}

static void start(tc_pool_t *p, int workers, size_t capacity)
{
    tc_err_t e =
        tc_pool_start(p, workers, capacity, 0, heap, STRING_LIT("pool"));
    ASSERT_EQ(TC_OK, e.kind);
}

/* --- start and stop ----------------------------------------------------- */

static void nothing(void *arg, allocator_t scratch)
{
    (void)arg;
    (void)scratch;
}

TEST(start_errors)
{
    tc_pool_t p;
    string_t n = STRING_LIT("pool");
    ASSERT_EQ(TC_INVALID, tc_pool_start(NULL, 1, 4, 0, heap, n).kind);
    ASSERT_EQ(TC_INVALID, tc_pool_start(&p, -1, 4, 0, heap, n).kind);
    ASSERT_EQ(TC_INVALID, tc_pool_start(&p, 1, 0, 0, heap, n).kind);
    ASSERT_EQ(TC_RESOURCE, tc_pool_start(&p, 1, 4, 0, ALLOCATOR_NULL, n).kind);
    /* room for the workers, not for the queue */
    ASSERT_EQ(
        TC_RESOURCE, tc_pool_start(&p, 1, sizeof backing, 0, heap, n).kind);
}

TEST(zero_workers_means_one_per_cpu)
{
    tc_pool_t p;
    start(&p, 0, 4);
    ASSERT_EQ(tc_cpu_count(), tc_pool_size(&p));
    tc_pool_stop(&p);
}

TEST(stop_gives_everything_back)
{
    debug_allocator_t d;
    debug_allocator_init(&d, heap);
    tc_pool_t p;
    ASSERT_EQ(
        TC_OK,
        tc_pool_start(
            &p, 3, 16, 0, debug_allocator_allocator(&d), STRING_LIT("pool"))
            .kind);
    ASSERT_EQ(3, tc_pool_size(&p));
    for (int i = 0; i < 100; ++i)
    {
        ASSERT_TRUE(tc_pool_submit(&p, nothing, NULL));
    }
    tc_pool_stop(&p);
    ASSERT_EQ(2, debug_allocator_stats(&d).alloc_count);
    ASSERT_EQ(0, debug_allocator_stats(&d).bytes_live);
}

/* --- every job runs once ------------------------------------------------ */

#define JOBS 10000

static atomic_int runs[JOBS];

static void count_run(void *arg, allocator_t scratch)
{
    (void)scratch;
    atomic_fetch_add(&runs[(int)(intptr_t)arg], 1);
}

TEST(every_job_runs_once)
{
    memset(runs, 0, sizeof runs);
    tc_pool_t p;
    start(&p, 4, 32);
    for (int i = 0; i < JOBS; ++i)
    {
        ASSERT_TRUE(tc_pool_submit(&p, count_run, (void *)(intptr_t)i));
    }
    tc_pool_stop(&p);
    int wrong = 0;
    for (int i = 0; i < JOBS; ++i)
    {
        wrong += atomic_load(&runs[i]) != 1;
    }
    ASSERT_EQ(0, wrong);
}

/* --- jobs run at the same time ------------------------------------------ */

#define WORKERS 4

typedef struct
{
    atomic_int arrived;
    atomic_int met; /* jobs that saw all WORKERS arrive */
} meeting_t;

/* Waits (up to 10 s) until WORKERS jobs are running at once. */
static void meet(void *arg, allocator_t scratch)
{
    (void)scratch;
    meeting_t *m = arg;
    atomic_fetch_add(&m->arrived, 1);
    uint64_t start_ns = tc_time_now_ns();
    while (atomic_load(&m->arrived) < WORKERS)
    {
        if (tc_time_now_ns() - start_ns > 10 * NS_PER_S)
        {
            return;
        }
        tc_thread_yield();
    }
    atomic_fetch_add(&m->met, 1);
}

TEST(jobs_run_on_every_worker_at_once)
{
    meeting_t m = {0};
    tc_pool_t p;
    start(&p, WORKERS, WORKERS);
    for (int i = 0; i < WORKERS; ++i)
    {
        ASSERT_TRUE(tc_pool_submit(&p, meet, &m));
    }
    tc_pool_stop(&p);
    ASSERT_EQ(WORKERS, atomic_load(&m.met));
}

/* --- stop and a full queue ---------------------------------------------- */

typedef struct
{
    tc_sema_t started; /* posted when the job begins      */
    tc_sema_t release; /* the job waits for this to finish */
    atomic_int done;
} gate_t;

static void gate_init(gate_t *g)
{
    tc_sema_init(&g->started, 0);
    tc_sema_init(&g->release, 0);
    atomic_init(&g->done, 0);
}

static void gate_destroy(gate_t *g)
{
    tc_sema_destroy(&g->release);
    tc_sema_destroy(&g->started);
}

static void held(void *arg, allocator_t scratch)
{
    (void)scratch;
    gate_t *g = arg;
    tc_sema_post(&g->started);
    tc_sema_wait(&g->release);
    atomic_fetch_add(&g->done, 1);
}

static void count_done(void *arg, allocator_t scratch)
{
    (void)scratch;
    atomic_fetch_add(&((gate_t *)arg)->done, 1);
}

/* With the one worker held: the queue (2) fills, a try fails, and stop
 * still runs everything that was queued. */
TEST(full_queue_and_stop_runs_what_is_queued)
{
    gate_t g;
    gate_init(&g);
    tc_pool_t p;
    start(&p, 1, 2);
    ASSERT_TRUE(tc_pool_submit(&p, held, &g));
    ASSERT_EQ(TC_OK, tc_sema_wait_timeout(&g.started, 10 * NS_PER_S));
    ASSERT_EQ(TC_OK, tc_pool_submit_timeout(&p, count_done, &g, 0));
    ASSERT_EQ(TC_OK, tc_pool_submit_timeout(&p, count_done, &g, 0));
    ASSERT_EQ(TC_TIMEOUT, tc_pool_submit_timeout(&p, count_done, &g, 0));
    uint64_t t0 = tc_time_now_ns();
    ASSERT_EQ(
        TC_TIMEOUT, tc_pool_submit_timeout(&p, count_done, &g, 20 * NS_PER_MS));
    ASSERT_GE(tc_time_now_ns() - t0, 20 * NS_PER_MS);
    ASSERT_EQ(0, atomic_load(&g.done));
    tc_sema_post(&g.release);
    tc_pool_stop(&p);
    ASSERT_EQ(3, atomic_load(&g.done));
    gate_destroy(&g);
}

/* --- scratch ------------------------------------------------------------ */

typedef struct
{
    size_t size;
    void *ptr;
    bool ok;
} scratch_use_t;

/* Allocates size bytes from scratch and fills them. */
static void use_scratch(void *arg, allocator_t scratch)
{
    scratch_use_t *u = arg;
    unsigned char *b = mem_alloc(scratch, u->size, 16);
    u->ptr = b;
    u->ok = b != NULL;
    if (b)
    {
        memset(b, 0xab, u->size);
        u->ok = b[0] == 0xab && b[u->size - 1] == 0xab;
    }
}

/* One worker: the second job gets the same memory back, because scratch
 * is reset after every job. */
TEST(scratch_is_reset_after_every_job)
{
    scratch_use_t a = {.size = 1000}, b = {.size = 1000};
    tc_pool_t p;
    start(&p, 1, 4);
    ASSERT_TRUE(tc_pool_submit(&p, use_scratch, &a));
    ASSERT_TRUE(tc_pool_submit(&p, use_scratch, &b));
    tc_pool_stop(&p);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    ASSERT_PTR_EQ(a.ptr, b.ptr);
}

/* A job may take far more than one block (64 KiB by default). */
TEST(scratch_grows)
{
    scratch_use_t u = {.size = 16 * 1024 * 1024};
    tc_pool_t p;
    start(&p, 1, 1);
    ASSERT_TRUE(tc_pool_submit(&p, use_scratch, &u));
    tc_pool_stop(&p);
    ASSERT_TRUE(u.ok);
}

/* --- jobs submitting jobs ----------------------------------------------- */

typedef struct
{
    tc_pool_t *pool;
    atomic_int runs;
    atomic_int refused;
} spawn_t;

/* Each job submits two more, up to depth 6: 1 + 2 + ... + 64 = 127 jobs,
 * as long as the queue has room (it is big enough here). */
typedef struct
{
    spawn_t *s;
    int depth;
} spawn_arg_t;

static spawn_arg_t args[128];
static atomic_int next_arg;

static void spawn(void *arg, allocator_t scratch)
{
    (void)scratch;
    spawn_arg_t *a = arg;
    atomic_fetch_add(&a->s->runs, 1);
    if (a->depth == 6)
    {
        return;
    }
    for (int i = 0; i < 2; ++i)
    {
        spawn_arg_t *child = &args[atomic_fetch_add(&next_arg, 1)];
        *child = (spawn_arg_t){a->s, a->depth + 1};
        if (tc_pool_submit_timeout(a->s->pool, spawn, child, 0) != TC_OK)
        {
            atomic_fetch_add(&a->s->refused, 1);
        }
    }
}

TEST(jobs_can_submit_jobs)
{
    tc_pool_t p;
    start(&p, 3, 128);
    spawn_t s = {.pool = &p};
    atomic_store(&next_arg, 1);
    args[0] = (spawn_arg_t){&s, 0};
    ASSERT_TRUE(tc_pool_submit(&p, spawn, &args[0]));
    /* wait for the tree to finish before stopping: stop closes the queue */
    uint64_t t0 = tc_time_now_ns();
    while (atomic_load(&s.runs) + atomic_load(&s.refused) < 127
           && tc_time_now_ns() - t0 < 10 * NS_PER_S)
    {
        tc_sleep_ns(NS_PER_MS);
    }
    tc_pool_stop(&p);
    ASSERT_EQ(0, atomic_load(&s.refused));
    ASSERT_EQ(127, atomic_load(&s.runs));
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc pool");
}
