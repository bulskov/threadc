/* Step 2: starting and joining threads.
 *
 *   tc_err_t tc_thread_start(tc_thread_t *t, tc_thread_fn fn, void *arg,
 *                            string_t name);
 *   void     tc_thread_join(tc_thread_t *t);
 *   uint64_t tc_thread_id(void);
 *   void     tc_thread_yield(void);
 *   int      tc_cpu_count(void);
 *
 * Rule for every test here: worker threads never ASSERT (ctt's test state
 * belongs to the main thread).  A worker writes what it saw into a struct;
 * the main thread checks it after tc_thread_join.  Join guarantees that
 * everything the thread wrote is visible afterwards — no atomics needed. */

#include "ctt.h"
#include "threadc/threadc.h"

#include <stdatomic.h>
#include <stdint.h>

#define NS_PER_MS UINT64_C(1000000)
#define MANY 16

/* --- helpers ---------------------------------------------------------------
 */

/* Each worker gets its own slot; nothing is shared between workers. */
typedef struct
{
    int input;    /* set by the main thread before start */
    int output;   /* set by the worker                   */
    uint64_t id;  /* tc_thread_id() seen by the worker   */
    uint64_t id2; /* a second call, to check stability   */
} slot_t;

static void record(void *arg)
{
    slot_t *s = arg;
    s->output = s->input * 2;
    s->id = tc_thread_id();
    tc_thread_yield();
    s->id2 = tc_thread_id();
}

/* A barrier from C11 atomics (mutexes arrive in step 3): every worker counts
 * itself in, then waits until all MANY have arrived.  So all workers are
 * alive at the same time before any of them records its id. */
static atomic_int arrived;

static void record_when_all_alive(void *arg)
{
    atomic_fetch_add(&arrived, 1);
    while (atomic_load(&arrived) < MANY)
        tc_thread_yield();
    record(arg);
}

static void sleep_then_mark(void *arg)
{
    tc_sleep_ns(50 * NS_PER_MS);
    *(int *)arg = 1;
}

static void do_nothing(void *arg)
{
    (void)arg;
}

/* --- start and join ---------------------------------------------------------
 */

TEST(start_returns_ok)
{
    tc_thread_t t;
    tc_err_t err = tc_thread_start(&t, do_nothing, NULL, STRING_LIT("t"));
    ASSERT_EQ(TC_OK, err.kind);
    ASSERT_EQ(0, err.os_code);
    tc_thread_join(&t);
}

TEST(thread_receives_its_arg)
{
    slot_t s = {.input = 21};
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, record, &s, STRING_LIT("w")).kind);
    tc_thread_join(&t);
    ASSERT_EQ(42, s.output);
}

/* Join must not return before the thread function has finished. */
TEST(join_waits_for_the_thread)
{
    int done = 0;
    tc_thread_t t;
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&t, sleep_then_mark, &done, STRING_LIT("s")).kind);
    tc_thread_join(&t);
    ASSERT_EQ(1, done);
    ASSERT_GE(tc_time_now_ns() - start, 50 * NS_PER_MS);
}

/* Many threads alive at once, each with its own arg.  Catches an
 * implementation that hands fn/arg to the new thread through a variable on
 * tc_thread_start's stack: the next start (or simply returning) overwrites
 * it before the thread reads it.  Under ASan (detect_stack_use_after_return)
 * that is reported even when the values happen to survive. */
TEST(many_threads_each_get_their_own_arg)
{
    slot_t slots[MANY] = {0};
    tc_thread_t threads[MANY];
    for (int i = 0; i < MANY; ++i)
    {
        slots[i].input = i + 1;
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&threads[i], record, &slots[i], STRING_LIT("many"))
                .kind);
    }
    for (int i = 0; i < MANY; ++i)
        tc_thread_join(&threads[i]);
    for (int i = 0; i < MANY; ++i)
        ASSERT_EQ((i + 1) * 2, slots[i].output);
}

/* Start/join many times through the same tc_thread_t: it is reusable after
 * join, and nothing leaks (LeakSanitizer in the asan job). */
TEST(thread_handle_is_reusable_after_join)
{
    tc_thread_t t;
    for (int i = 0; i < 200; ++i)
    {
        slot_t s = {.input = i};
        ASSERT_EQ(TC_OK, tc_thread_start(&t, record, &s, STRING_LIT("r")).kind);
        tc_thread_join(&t);
        ASSERT_EQ(i * 2, s.output);
    }
}

/* --- names ------------------------------------------------------------------
 */
/* Whether the name is visible to debuggers is step 6.  Here: every kind of
 * name is accepted. */

TEST(empty_name_is_accepted)
{
    tc_thread_t t;
    ASSERT_EQ(
        TC_OK, tc_thread_start(&t, do_nothing, NULL, STRING_LIT("")).kind);
    tc_thread_join(&t);
}

TEST(null_name_is_accepted)
{
    tc_thread_t t;
    string_t none = {NULL, 0};
    ASSERT_EQ(TC_OK, tc_thread_start(&t, do_nothing, NULL, none).kind);
    tc_thread_join(&t);
}

/* Longer than any OS limit (Linux: 15 bytes): truncated, not rejected. */
TEST(long_name_is_accepted)
{
    tc_thread_t t;
    string_t name = STRING_LIT("a-thread-name-much-longer-than-fifteen-bytes");
    ASSERT_EQ(TC_OK, tc_thread_start(&t, do_nothing, NULL, name).kind);
    tc_thread_join(&t);
}

/* The name is copied: the caller's buffer may die right after start.  (Only
 * really exercised once names are applied, in step 6.) */
TEST(name_buffer_may_change_after_start)
{
    char buf[] = "temporary";
    tc_thread_t t;
    string_t name = {buf, sizeof buf - 1};
    ASSERT_EQ(TC_OK, tc_thread_start(&t, do_nothing, NULL, name).kind);
    for (size_t i = 0; i < sizeof buf - 1; ++i)
        buf[i] = 'x';
    tc_thread_join(&t);
}

/* --- invalid arguments -------------------------------------------------------
 */

TEST(null_thread_is_invalid)
{
    tc_err_t err = tc_thread_start(NULL, do_nothing, NULL, STRING_LIT("x"));
    ASSERT_EQ(TC_INVALID, err.kind);
}

TEST(null_function_is_invalid)
{
    tc_thread_t t;
    tc_err_t err = tc_thread_start(&t, NULL, NULL, STRING_LIT("x"));
    ASSERT_EQ(TC_INVALID, err.kind);
}

/* --- tc_thread_id
 * ------------------------------------------------------------- */

TEST(thread_id_is_stable_within_a_thread)
{
    ASSERT_EQ(tc_thread_id(), tc_thread_id());

    slot_t s = {0};
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, record, &s, STRING_LIT("id")).kind);
    tc_thread_join(&t);
    ASSERT_EQ(s.id, s.id2);
}

/* All threads alive at the same time have distinct ids, and none equals the
 * main thread's.  Ids of finished threads may be reused by the OS, so the
 * workers wait for each other (record_when_all_alive) before recording. */
TEST(live_threads_have_distinct_ids)
{
    slot_t slots[MANY] = {0};
    tc_thread_t threads[MANY];
    atomic_store(&arrived, 0);
    for (int i = 0; i < MANY; ++i)
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(
                &threads[i],
                record_when_all_alive,
                &slots[i],
                STRING_LIT("ids"))
                .kind);
    for (int i = 0; i < MANY; ++i)
        tc_thread_join(&threads[i]);

    uint64_t main_id = tc_thread_id();
    for (int i = 0; i < MANY; ++i)
    {
        ASSERT_NE(main_id, slots[i].id);
        for (int j = i + 1; j < MANY; ++j)
            ASSERT_NE(slots[i].id, slots[j].id);
    }
}

/* --- tc_cpu_count, tc_thread_yield
 * --------------------------------------------- */

TEST(cpu_count_is_at_least_one)
{
    ASSERT_GE(tc_cpu_count(), 1);
}

TEST(yield_returns)
{
    for (int i = 0; i < 1000; ++i)
        tc_thread_yield();
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc thread");
}
