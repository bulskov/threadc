/* Step 3: mutexes.
 *
 *   void tc_mutex_init(tc_mutex_t *m);
 *   void tc_mutex_destroy(tc_mutex_t *m);
 *   void tc_mutex_lock(tc_mutex_t *m);
 *   bool tc_mutex_try_lock(tc_mutex_t *m);   true = acquired
 *   void tc_mutex_unlock(tc_mutex_t *m);
 *
 * A mutex guarantees two things, and the tests check both:
 *   1. mutual exclusion — one thread at a time in the critical section;
 *   2. visibility — what a thread wrote before unlock is seen by the next
 *      thread after lock (so a plain `long counter` is enough).
 *
 * As in thread_test.c, workers never ASSERT: they record, the main thread
 * checks after join.
 *
 * EXPERIMENT (do it once, then undo it): comment out the tc_mutex_lock and
 * tc_mutex_unlock calls in increment_with_lock below and run
 *     ./test.sh tsan
 * ThreadSanitizer reports "WARNING: ThreadSanitizer: data race" with the
 * two stack traces that collided — that is what a real race looks like.
 * Under plain ./test.sh the counter test fails most of the time instead,
 * because increments get lost. */

#include "ctt.h"
#include "threadc/threadc.h"

#define THREADS 8
#define INCREMENTS 50000

/* --- shared state ------------------------------------------------------------
 */

typedef struct
{
    tc_mutex_t mutex;
    long counter;   /* protected by mutex                                 */
    int inside;     /* threads currently in the critical section          */
    int max_inside; /* the most ever seen at once — must stay 1           */
} shared_t;

/* The critical section: count ourselves in, check nobody else is there,
 * do the work, count ourselves out.  If the mutex fails to exclude,
 * max_inside becomes 2 or more. */
static void critical_section(shared_t *s)
{
    s->inside++;
    if (s->inside > s->max_inside)
        s->max_inside = s->inside;
    s->counter++;
    s->inside--;
}

static void increment_with_lock(void *arg)
{
    shared_t *s = arg;
    for (int i = 0; i < INCREMENTS; ++i)
    {
        tc_mutex_lock(&s->mutex);
        critical_section(s);
        tc_mutex_unlock(&s->mutex);
    }
}

/* Same, but spinning on try_lock: it must exclude just as well. */
static void increment_with_try_lock(void *arg)
{
    shared_t *s = arg;
    for (int i = 0; i < INCREMENTS; ++i)
    {
        while (!tc_mutex_try_lock(&s->mutex))
            tc_thread_yield();
        critical_section(s);
        tc_mutex_unlock(&s->mutex);
    }
}

/* Start THREADS workers on the same shared_t and wait for them all. */
static void run_workers(tc_thread_fn fn, shared_t *s)
{
    tc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i)
        ASSERT_EQ(
            TC_OK, tc_thread_start(&threads[i], fn, s, STRING_LIT("w")).kind);
    for (int i = 0; i < THREADS; ++i)
        tc_thread_join(&threads[i]);
}

/* --- single thread
 * ------------------------------------------------------------ */

TEST(init_lock_unlock_destroy)
{
    tc_mutex_t m;
    tc_mutex_init(&m);
    tc_mutex_lock(&m);
    tc_mutex_unlock(&m);
    tc_mutex_destroy(&m);
}

/* Lots of mutexes created and destroyed: nothing leaks (LeakSanitizer in the
 * asan job), and a destroyed mutex's storage can be initialised again. */
TEST(init_destroy_many_times)
{
    tc_mutex_t m;
    for (int i = 0; i < 1000; ++i)
    {
        tc_mutex_init(&m);
        tc_mutex_lock(&m);
        tc_mutex_unlock(&m);
        tc_mutex_destroy(&m);
    }
}

TEST(try_lock_on_a_free_mutex_succeeds)
{
    tc_mutex_t m;
    tc_mutex_init(&m);
    ASSERT_TRUE(tc_mutex_try_lock(&m));
    tc_mutex_unlock(&m);
    tc_mutex_destroy(&m);
}

/* A mutex can live inside another struct (that is why tc_mutex_t is fixed
 * storage, not a pointer). */
TEST(mutex_embedded_in_a_struct)
{
    shared_t s = {0};
    tc_mutex_init(&s.mutex);
    tc_mutex_lock(&s.mutex);
    s.counter = 7;
    tc_mutex_unlock(&s.mutex);
    tc_mutex_destroy(&s.mutex);
    ASSERT_EQ(7, s.counter);
}

/* --- try_lock against another thread ----------------------------------------
 */

typedef struct
{
    tc_mutex_t *mutex;
    bool acquired;
} try_result_t;

static void try_once(void *arg)
{
    try_result_t *r = arg;
    r->acquired = tc_mutex_try_lock(r->mutex);
    if (r->acquired)
        tc_mutex_unlock(r->mutex);
}

TEST(try_lock_fails_while_another_thread_holds_it)
{
    tc_mutex_t m;
    tc_mutex_init(&m);
    try_result_t r = {.mutex = &m};
    tc_thread_t t;

    tc_mutex_lock(&m); /* main holds it ... */
    ASSERT_EQ(TC_OK, tc_thread_start(&t, try_once, &r, STRING_LIT("try")).kind);
    tc_thread_join(&t);
    tc_mutex_unlock(&m);
    ASSERT_FALSE(r.acquired); /* ... so the worker could not get it */

    /* Released: now the worker gets it. */
    ASSERT_EQ(TC_OK, tc_thread_start(&t, try_once, &r, STRING_LIT("try")).kind);
    tc_thread_join(&t);
    ASSERT_TRUE(r.acquired);

    tc_mutex_destroy(&m);
}

/* --- many threads
 * ----------------------------------------------------------------- */

TEST(lock_makes_increments_exact)
{
    shared_t s = {0};
    tc_mutex_init(&s.mutex);
    run_workers(increment_with_lock, &s);
    tc_mutex_destroy(&s.mutex);

    ASSERT_EQ((long)THREADS * INCREMENTS, s.counter);
    ASSERT_EQ(1, s.max_inside);
}

TEST(try_lock_makes_increments_exact)
{
    shared_t s = {0};
    tc_mutex_init(&s.mutex);
    run_workers(increment_with_try_lock, &s);
    tc_mutex_destroy(&s.mutex);

    ASSERT_EQ((long)THREADS * INCREMENTS, s.counter);
    ASSERT_EQ(1, s.max_inside);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc mutex");
}
