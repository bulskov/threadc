/* Step 5: run-once initialisation.
 *
 *   void tc_once(tc_once_t *o, void (*fn)(void));
 *   #define TC_ONCE_INIT {{0}}
 *
 * fn runs exactly once per tc_once_t, however many threads call tc_once at
 * the same moment, and every call returns only after fn has finished — so
 * whatever fn initialised is ready to use.  A tc_once_t is valid when it is
 * all zeros: TC_ONCE_INIT, static storage, or memset. */

#include "ctt.h"
#include "threadc/threadc.h"

#include <stdatomic.h>
#include <string.h>

#define THREADS 16
#define NS_PER_MS 1000000

/* --- what the init functions touch -------------------------------------- */

static atomic_int calls; /* how many times an init function ran       */
static int initialised;  /* plain int: written by fn, read by callers */

static void init_slow(void)
{
    atomic_fetch_add(&calls, 1);
    tc_sleep_ns(20 * NS_PER_MS); /* let the other threads pile up */
    initialised = 42;
}

static void init_fast(void)
{
    atomic_fetch_add(&calls, 1);
}

static void reset(void)
{
    atomic_store(&calls, 0);
    initialised = 0;
}

/* --- single thread ------------------------------------------------------ */

TEST(runs_fn_the_first_time)
{
    reset();
    tc_once_t once = TC_ONCE_INIT;
    tc_once(&once, init_fast);
    ASSERT_EQ(1, atomic_load(&calls));
}

TEST(later_calls_do_nothing)
{
    reset();
    tc_once_t once = TC_ONCE_INIT;
    for (int i = 0; i < 100; ++i)
        tc_once(&once, init_fast);
    ASSERT_EQ(1, atomic_load(&calls));
}

TEST(separate_once_objects_are_independent)
{
    reset();
    tc_once_t a = TC_ONCE_INIT;
    tc_once_t b = TC_ONCE_INIT;
    tc_once(&a, init_fast);
    tc_once(&b, init_fast);
    tc_once(&a, init_fast);
    ASSERT_EQ(2, atomic_load(&calls));
}

/* All zeros is the initial state, however the zeros got there. */
static tc_once_t static_once; /* zero-initialised, no TC_ONCE_INIT */

TEST(static_and_memset_once_objects_work)
{
    reset();
    tc_once(&static_once, init_fast);
    tc_once(&static_once, init_fast);

    tc_once_t zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    tc_once(&zeroed, init_fast);

    ASSERT_EQ(2, atomic_load(&calls));
}

/* --- many threads ------------------------------------------------------- */

typedef struct
{
    tc_once_t *once;
    int seen; /* `initialised` as the caller saw it right after tc_once */
} caller_t;

static void call_once(void *arg)
{
    caller_t *c = arg;
    tc_once(c->once, init_slow);
    c->seen = initialised; /* plain read: tc_once must make it visible */
}

/* 16 threads race into tc_once while init_slow takes 20 ms: it runs once,
 * and nobody returns before it has finished (every caller sees 42).
 * Under TSan, a missing acquire/release shows up as a data race on
 * `initialised`. */
TEST(racing_threads_run_fn_once_and_all_see_its_result)
{
    reset();
    tc_once_t once = TC_ONCE_INIT;
    caller_t callers[THREADS];
    tc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        callers[i] = (caller_t){.once = &once};
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(
                &threads[i], call_once, &callers[i], STRING_LIT("once"))
                .kind);
    }
    for (int i = 0; i < THREADS; ++i)
        tc_thread_join(&threads[i]);

    ASSERT_EQ(1, atomic_load(&calls));
    for (int i = 0; i < THREADS; ++i)
        ASSERT_EQ(42, callers[i].seen);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc once");
}
