/* Bounded MPMC queue.
 *
 *   tc_err_t      tc_queue_init(tc_queue_t *q, size_t elem_size,
 *                               size_t capacity, allocator_t a);
 *   void          tc_queue_destroy(tc_queue_t *q);
 *   bool          tc_queue_push(tc_queue_t *q, const void *elem);
 *   bool          tc_queue_pop(tc_queue_t *q, void *out);
 *   tc_err_kind_t tc_queue_push_timeout(q, elem, timeout_ns);
 *   tc_err_kind_t tc_queue_pop_timeout(q, out, timeout_ns);
 *   void          tc_queue_close(tc_queue_t *q);
 *   size_t        tc_queue_len(tc_queue_t *q);
 *
 * Workers never ASSERT: they record, the main thread checks after join. */

#include "ctt.h"
#include "threadc/queue.h"

#include "arena/debug_allocator.h"
#include "arena/fixed_arena.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define NS_PER_MS UINT64_C(1000000)

static unsigned char backing[1 << 16];
static fixed_arena_t arena;
static allocator_t heap;

void ctt_before_each(void)
{
    fixed_arena_init(&arena, backing, sizeof backing);
    heap = fixed_arena_allocator(&arena);
}

static tc_queue_t make_queue(size_t elem_size, size_t capacity)
{
    tc_queue_t q;
    tc_err_t e = tc_queue_init(&q, elem_size, capacity, heap);
    CTT_ASSERT_EQ(TC_OK, e.kind);
    return q;
}

/* --- init and destroy --------------------------------------------------- */

TEST(init_errors)
{
    tc_queue_t q;
    ASSERT_EQ(TC_INVALID, tc_queue_init(NULL, 4, 4, heap).kind);
    ASSERT_EQ(TC_INVALID, tc_queue_init(&q, 0, 4, heap).kind);
    ASSERT_EQ(TC_INVALID, tc_queue_init(&q, 4, 0, heap).kind);
    ASSERT_EQ(TC_INVALID, tc_queue_init(&q, 16, SIZE_MAX / 8, heap).kind);
    ASSERT_EQ(TC_RESOURCE, tc_queue_init(&q, 4, 4, ALLOCATOR_NULL).kind);
    ASSERT_EQ(TC_RESOURCE, tc_queue_init(&q, 1, sizeof backing + 1, heap).kind);
}

/* The ring buffer is the only allocation, and destroy gives it back. */
TEST(destroy_gives_the_buffer_back)
{
    debug_allocator_t d;
    debug_allocator_init(&d, heap);
    tc_queue_t q;
    ASSERT_EQ(
        TC_OK, tc_queue_init(&q, 8, 10, debug_allocator_allocator(&d)).kind);
    ASSERT_EQ(1, debug_allocator_stats(&d).alloc_count);
    ASSERT_EQ(80, debug_allocator_stats(&d).bytes_live);
    uint64_t x = 1;
    ASSERT_TRUE(tc_queue_push(&q, &x)); /* no allocation per element */
    ASSERT_TRUE(tc_queue_pop(&q, &x));
    ASSERT_EQ(1, debug_allocator_stats(&d).alloc_count);
    tc_queue_destroy(&q);
    ASSERT_EQ(0, debug_allocator_stats(&d).bytes_live);
}

/* --- one thread --------------------------------------------------------- */

TEST(fifo_order_around_the_ring)
{
    tc_queue_t q = make_queue(sizeof(int), 4);
    int next_in = 0, next_out = 0;
    /* 3 in, 3 out: the oldest element moves 3 slots of 4 each round, so it
     * crosses the end of the ring most rounds */
    for (int round = 0; round < 50; ++round)
    {
        for (int i = 0; i < 3; ++i, ++next_in)
        {
            ASSERT_TRUE(tc_queue_push(&q, &next_in));
        }
        ASSERT_EQ(3, tc_queue_len(&q));
        for (int i = 0; i < 3; ++i, ++next_out)
        {
            int v = -1;
            ASSERT_TRUE(tc_queue_pop(&q, &v));
            ASSERT_EQ(next_out, v);
        }
    }
    ASSERT_EQ(150, next_out);
    tc_queue_destroy(&q);
}

typedef struct
{
    char c[3];
} odd_t;

typedef struct
{
    uint64_t a, b, c, d, e;
} wide_t;

TEST(elements_of_any_size)
{
    tc_queue_t q = make_queue(sizeof(odd_t), 3);
    for (int i = 0; i < 7; ++i)
    {
        odd_t in = {{(char)i, (char)(i + 1), (char)(i + 2)}}, out = {{0}};
        ASSERT_TRUE(tc_queue_push(&q, &in));
        ASSERT_TRUE(tc_queue_pop(&q, &out));
        ASSERT_EQ(0, memcmp(&in, &out, sizeof in));
    }
    tc_queue_destroy(&q);

    q = make_queue(sizeof(wide_t), 2);
    wide_t w = {1, 2, 3, 4, 5}, r = {0};
    ASSERT_TRUE(tc_queue_push(&q, &w));
    ASSERT_TRUE(tc_queue_pop(&q, &r));
    ASSERT_EQ(5, r.e);
    ASSERT_EQ(1, r.a);
    tc_queue_destroy(&q);
}

TEST(zero_timeout_only_tries)
{
    tc_queue_t q = make_queue(sizeof(int), 2);
    int v = 7;
    ASSERT_EQ(TC_OK, tc_queue_push_timeout(&q, &v, 0));
    ASSERT_EQ(TC_OK, tc_queue_push_timeout(&q, &v, 0));
    ASSERT_EQ(TC_TIMEOUT, tc_queue_push_timeout(&q, &v, 0));
    ASSERT_EQ(2, tc_queue_len(&q));
    ASSERT_EQ(TC_OK, tc_queue_pop_timeout(&q, &v, 0));
    ASSERT_EQ(TC_OK, tc_queue_pop_timeout(&q, &v, 0));
    ASSERT_EQ(TC_TIMEOUT, tc_queue_pop_timeout(&q, &v, 0));
    ASSERT_EQ(0, tc_queue_len(&q));
    tc_queue_destroy(&q);
}

TEST(timeouts_wait_the_full_time)
{
    tc_queue_t q = make_queue(sizeof(int), 1);
    int v = 1;
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(TC_TIMEOUT, tc_queue_pop_timeout(&q, &v, 30 * NS_PER_MS));
    ASSERT_GE(tc_time_now_ns() - start, 30 * NS_PER_MS);

    ASSERT_TRUE(tc_queue_push(&q, &v));
    start = tc_time_now_ns();
    ASSERT_EQ(TC_TIMEOUT, tc_queue_push_timeout(&q, &v, 30 * NS_PER_MS));
    ASSERT_GE(tc_time_now_ns() - start, 30 * NS_PER_MS);
    tc_queue_destroy(&q);
}

/* Close: push fails at once, pop drains what is left, then fails. */
TEST(close_drains_then_fails)
{
    tc_queue_t q = make_queue(sizeof(int), 4);
    int a = 1, b = 2, v = 0;
    ASSERT_TRUE(tc_queue_push(&q, &a));
    ASSERT_TRUE(tc_queue_push(&q, &b));
    tc_queue_close(&q);
    tc_queue_close(&q); /* again: nothing happens */
    ASSERT_FALSE(tc_queue_push(&q, &a));
    ASSERT_EQ(TC_CLOSED, tc_queue_push_timeout(&q, &a, 0));
    ASSERT_TRUE(tc_queue_pop(&q, &v));
    ASSERT_EQ(1, v);
    ASSERT_EQ(TC_OK, tc_queue_pop_timeout(&q, &v, 0));
    ASSERT_EQ(2, v);
    v = 99;
    ASSERT_FALSE(tc_queue_pop(&q, &v));
    ASSERT_EQ(TC_CLOSED, tc_queue_pop_timeout(&q, &v, 0));
    ASSERT_EQ(99, v); /* untouched */
    tc_queue_destroy(&q);
}

/* --- waiting threads ---------------------------------------------------- */

typedef struct
{
    tc_queue_t *q;
    int value;
    bool ok;
    atomic_bool done;
} job_t;

static void push_one(void *arg)
{
    job_t *j = arg;
    j->ok = tc_queue_push(j->q, &j->value);
    atomic_store(&j->done, true);
}

static void pop_one(void *arg)
{
    job_t *j = arg;
    j->ok = tc_queue_pop(j->q, &j->value);
    atomic_store(&j->done, true);
}

TEST(push_waits_for_room)
{
    tc_queue_t q = make_queue(sizeof(int), 1);
    int v = 1;
    ASSERT_TRUE(tc_queue_push(&q, &v));
    job_t j = {.q = &q, .value = 2};
    tc_thread_t t;
    ASSERT_EQ(
        TC_OK, tc_thread_start(&t, push_one, &j, STRING_LIT("push")).kind);
    tc_sleep_ns(20 * NS_PER_MS);
    ASSERT_FALSE(atomic_load(&j.done));
    ASSERT_TRUE(tc_queue_pop(&q, &v));
    ASSERT_EQ(1, v);
    tc_thread_join(&t);
    ASSERT_TRUE(j.ok);
    ASSERT_TRUE(tc_queue_pop(&q, &v));
    ASSERT_EQ(2, v);
    tc_queue_destroy(&q);
}

TEST(pop_waits_for_an_element)
{
    tc_queue_t q = make_queue(sizeof(int), 1);
    job_t j = {.q = &q};
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, pop_one, &j, STRING_LIT("pop")).kind);
    tc_sleep_ns(20 * NS_PER_MS);
    ASSERT_FALSE(atomic_load(&j.done));
    int v = 42;
    ASSERT_TRUE(tc_queue_push(&q, &v));
    tc_thread_join(&t);
    ASSERT_TRUE(j.ok);
    ASSERT_EQ(42, j.value);
    tc_queue_destroy(&q);
}

#define WAITERS 4

/* Close wakes every waiting thread, on both sides. */
static void check_close_wakes(tc_thread_fn fn, size_t fill)
{
    tc_queue_t q = make_queue(sizeof(int), 2);
    int v = 0;
    for (size_t i = 0; i < fill; ++i)
    {
        ASSERT_TRUE(tc_queue_push(&q, &v));
    }
    job_t j[WAITERS] = {{0}};
    tc_thread_t t[WAITERS];
    for (int i = 0; i < WAITERS; ++i)
    {
        j[i].q = &q;
        j[i].ok = true;
        ASSERT_EQ(
            TC_OK, tc_thread_start(&t[i], fn, &j[i], STRING_LIT("w")).kind);
    }
    tc_sleep_ns(20 * NS_PER_MS);
    tc_queue_close(&q);
    for (int i = 0; i < WAITERS; ++i)
    {
        tc_thread_join(&t[i]);
        ASSERT_FALSE(j[i].ok);
    }
    ASSERT_EQ(fill, tc_queue_len(&q));
    tc_queue_destroy(&q);
}

TEST(close_wakes_waiting_poppers)
{
    check_close_wakes(pop_one, 0);
}

TEST(close_wakes_waiting_pushers)
{
    check_close_wakes(push_one, 2);
}

/* --- many producers, many consumers ------------------------------------- */

#define PRODUCERS 4
#define CONSUMERS 4
#define PER_PRODUCER 20000
#define TOTAL (PRODUCERS * PER_PRODUCER)

typedef struct
{
    tc_queue_t *q;
    int id;
    /* consumer: how often each value was seen, and whether the values of
     * each producer came in the order they were pushed */
    unsigned char seen[TOTAL];
    int last[PRODUCERS];
    bool in_order;
} worker_t;

static void produce(void *arg)
{
    worker_t *w = arg;
    for (int i = 0; i < PER_PRODUCER; ++i)
    {
        int v = w->id * PER_PRODUCER + i;
        if (!tc_queue_push(w->q, &v))
        {
            return; /* closed early: the main thread sees the gaps */
        }
    }
}

static void consume(void *arg)
{
    worker_t *w = arg;
    w->in_order = true;
    for (int p = 0; p < PRODUCERS; ++p)
    {
        w->last[p] = -1;
    }
    int v;
    while (tc_queue_pop(w->q, &v))
    {
        if (v < 0 || v >= TOTAL)
        {
            w->in_order = false;
            continue;
        }
        w->seen[v]++;
        int p = v / PER_PRODUCER;
        if (v <= w->last[p])
        {
            w->in_order = false;
        }
        w->last[p] = v;
    }
}

static worker_t producers[PRODUCERS], consumers[CONSUMERS];

/* Every element arrives exactly once, and each consumer sees each
 * producer's elements in the order they were pushed (FIFO). */
TEST(every_element_once_in_order)
{
    tc_queue_t q = make_queue(sizeof(int), 8);
    tc_thread_t pt[PRODUCERS], ct[CONSUMERS];
    for (int i = 0; i < CONSUMERS; ++i)
    {
        consumers[i] = (worker_t){.q = &q, .id = i};
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&ct[i], consume, &consumers[i], STRING_LIT("c"))
                .kind);
    }
    for (int i = 0; i < PRODUCERS; ++i)
    {
        producers[i] = (worker_t){.q = &q, .id = i};
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&pt[i], produce, &producers[i], STRING_LIT("p"))
                .kind);
    }
    for (int i = 0; i < PRODUCERS; ++i)
    {
        tc_thread_join(&pt[i]);
    }
    tc_queue_close(&q);
    for (int i = 0; i < CONSUMERS; ++i)
    {
        tc_thread_join(&ct[i]);
    }
    tc_queue_destroy(&q);

    int missing = 0, twice = 0;
    for (int v = 0; v < TOTAL; ++v)
    {
        int n = 0;
        for (int c = 0; c < CONSUMERS; ++c)
        {
            n += consumers[c].seen[v];
        }
        missing += n == 0;
        twice += n > 1;
    }
    ASSERT_EQ(0, missing);
    ASSERT_EQ(0, twice);
    for (int c = 0; c < CONSUMERS; ++c)
    {
        ASSERT_TRUE(consumers[c].in_order);
    }
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc queue");
}
