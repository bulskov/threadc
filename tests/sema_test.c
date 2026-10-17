/* Semaphore.
 *
 *   void          tc_sema_init(tc_sema_t *s, uint64_t count);
 *   void          tc_sema_destroy(tc_sema_t *s);
 *   void          tc_sema_wait(tc_sema_t *s);
 *   tc_err_kind_t tc_sema_wait_timeout(tc_sema_t *s, uint64_t timeout_ns);
 *   void          tc_sema_post(tc_sema_t *s);
 *
 * Workers never ASSERT: they record, the main thread checks after join. */

#include "ctt.h"
#include "threadc/sema.h"

#include <stdatomic.h>
#include <stdint.h>

#define NS_PER_MS UINT64_C(1000000)

/* --- one unit at a time ------------------------------------------------- */

TEST(starts_with_its_count)
{
    tc_sema_t s;
    tc_sema_init(&s, 2);
    ASSERT_EQ(TC_OK, tc_sema_wait_timeout(&s, 0));
    ASSERT_EQ(TC_OK, tc_sema_wait_timeout(&s, 0));
    ASSERT_EQ(TC_TIMEOUT, tc_sema_wait_timeout(&s, 0));
    tc_sema_post(&s);
    ASSERT_EQ(TC_OK, tc_sema_wait_timeout(&s, 0));
    tc_sema_destroy(&s);
}

TEST(wait_takes_a_unit_that_is_there)
{
    tc_sema_t s;
    tc_sema_init(&s, 1);
    tc_sema_wait(&s); /* must not block */
    ASSERT_EQ(TC_TIMEOUT, tc_sema_wait_timeout(&s, 0));
    tc_sema_destroy(&s);
}

TEST(timeout_waits_the_full_time)
{
    tc_sema_t s;
    tc_sema_init(&s, 0);
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(TC_TIMEOUT, tc_sema_wait_timeout(&s, 50 * NS_PER_MS));
    ASSERT_GE(tc_time_now_ns() - start, 50 * NS_PER_MS);
    tc_sema_destroy(&s);
}

/* --- waking a waiter ---------------------------------------------------- */

typedef struct
{
    tc_sema_t sema;
    uint64_t timeout; /* TC_FOREVER: tc_sema_wait */
    atomic_bool done;
    tc_err_kind_t result;
} waiter_t;

static void wait_once(void *arg)
{
    waiter_t *w = arg;
    if (w->timeout == TC_FOREVER)
    {
        tc_sema_wait(&w->sema);
        w->result = TC_OK;
    }
    else
    {
        w->result = tc_sema_wait_timeout(&w->sema, w->timeout);
    }
    atomic_store(&w->done, true);
}

/* The waiter is still waiting after 20 ms, and a post lets it go. */
static void check_post_wakes(uint64_t timeout)
{
    waiter_t w = {.timeout = timeout};
    tc_sema_init(&w.sema, 0);
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, wait_once, &w, STRING_LIT("w")).kind);
    tc_sleep_ns(20 * NS_PER_MS);
    ASSERT_FALSE(atomic_load(&w.done));
    tc_sema_post(&w.sema);
    tc_thread_join(&t);
    ASSERT_TRUE(atomic_load(&w.done));
    ASSERT_EQ(TC_OK, w.result);
    ASSERT_EQ(TC_TIMEOUT, tc_sema_wait_timeout(&w.sema, 0));
    tc_sema_destroy(&w.sema);
}

TEST(post_wakes_wait)
{
    check_post_wakes(TC_FOREVER);
}

TEST(post_wakes_wait_timeout)
{
    check_post_wakes(10 * UINT64_C(1000000000));
}

/* --- many threads ------------------------------------------------------- */

#define THREADS 4
#define UNITS 20000

static void post_many(void *arg)
{
    for (int i = 0; i < UNITS; ++i)
    {
        tc_sema_post(arg);
    }
}

static void wait_many(void *arg)
{
    for (int i = 0; i < UNITS; ++i)
    {
        tc_sema_wait(arg);
    }
}

/* Every unit posted is taken exactly once: the waiters finish, and nothing
 * is left over. */
TEST(every_post_is_taken_once)
{
    tc_sema_t s;
    tc_sema_init(&s, 0);
    tc_thread_t waiters[THREADS], posters[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&waiters[i], wait_many, &s, STRING_LIT("w")).kind);
    }
    for (int i = 0; i < THREADS; ++i)
    {
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&posters[i], post_many, &s, STRING_LIT("p")).kind);
    }
    for (int i = 0; i < THREADS; ++i)
    {
        tc_thread_join(&posters[i]);
        tc_thread_join(&waiters[i]);
    }
    ASSERT_EQ(TC_TIMEOUT, tc_sema_wait_timeout(&s, 0));
    tc_sema_destroy(&s);
}

/* A semaphore of one unit excludes like a mutex. */

#define INCREMENTS 20000

typedef struct
{
    tc_sema_t sema;
    long counter;
    int inside;
    int max_inside;
} shared_t;

static void increment(void *arg)
{
    shared_t *s = arg;
    for (int i = 0; i < INCREMENTS; ++i)
    {
        tc_sema_wait(&s->sema);
        s->inside++;
        if (s->inside > s->max_inside)
        {
            s->max_inside = s->inside;
        }
        s->counter++;
        s->inside--;
        tc_sema_post(&s->sema);
    }
}

TEST(one_unit_excludes)
{
    shared_t s = {0};
    tc_sema_init(&s.sema, 1);
    tc_thread_t t[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        ASSERT_EQ(
            TC_OK, tc_thread_start(&t[i], increment, &s, STRING_LIT("i")).kind);
    }
    for (int i = 0; i < THREADS; ++i)
    {
        tc_thread_join(&t[i]);
    }
    tc_sema_destroy(&s.sema);
    ASSERT_EQ((long)THREADS * INCREMENTS, s.counter);
    ASSERT_EQ(1, s.max_inside);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc sema");
}
