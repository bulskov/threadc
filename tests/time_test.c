/* Step 1: the monotonic clock and sleep.
 *
 *   uint64_t tc_time_now_ns(void);   monotonic nanoseconds, arbitrary origin
 *   void     tc_sleep_ns(uint64_t);  sleep for AT LEAST the given time
 *
 * Only differences between two tc_time_now_ns() values mean anything.
 * Timing tests run on shared, slow CI machines, so they check lower bounds
 * and generous upper bounds — never "exactly". */

#include "ctt.h"
#include "threadc/threadc.h"

#include <stdint.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_S UINT64_C(1000000000)

/* Upper bound for "this should be quick": far above any real scheduling
 * delay, far below what a unit mistake produces (e.g. treating ns as ms
 * turns a 10 ms sleep into ~2.8 hours). */
#define GENEROUS (1 * NS_PER_S)

/* How long a tc_sleep_ns(ns) call actually took, measured with the clock
 * under test. */
static uint64_t measure_sleep(uint64_t ns)
{
    uint64_t start = tc_time_now_ns();
    tc_sleep_ns(ns);
    return tc_time_now_ns() - start;
}

/* --- tc_time_now_ns ------------------------------------------------------ */

TEST(now_never_goes_backwards)
{
    uint64_t prev = tc_time_now_ns();
    for (int i = 0; i < 100000; ++i)
    {
        uint64_t now = tc_time_now_ns();
        ASSERT_GE(now, prev);
        prev = now;
    }
}

/* Catches a clock that returns a constant (or a wrong field): spin until it
 * changes, and give up after a generous amount of work. */
TEST(now_advances)
{
    uint64_t start = tc_time_now_ns();
    uint64_t now = start;
    for (long i = 0; i < 100000000 && now == start; ++i)
    {
        now = tc_time_now_ns();
    }
    ASSERT_GT(now, start);
}

/* The clock and sleep agree on units: two readings around a 50 ms sleep are
 * at least 50 ms apart. */
TEST(now_measures_nanoseconds)
{
    uint64_t elapsed = measure_sleep(50 * NS_PER_MS);
    ASSERT_GE(elapsed, 50 * NS_PER_MS);
    ASSERT_LT(elapsed, GENEROUS);
}

/* --- tc_sleep_ns ---------------------------------------------------------- */

TEST(sleep_lasts_at_least_the_requested_time)
{
    ASSERT_GE(measure_sleep(10 * NS_PER_MS), 10 * NS_PER_MS);
}

TEST(sleep_is_not_absurdly_long)
{
    ASSERT_LT(measure_sleep(10 * NS_PER_MS), GENEROUS);
}

/* 1.5 ms: catches rounding DOWN to whole milliseconds (Sleep() on Windows
 * takes milliseconds — 1.5 ms must become 2, not 1). */
TEST(sleep_rounds_sub_millisecond_up)
{
    ASSERT_GE(measure_sleep(1500 * 1000), 1500 * 1000);
}

/* 1.1 s: catches a timespec built with tv_nsec >= 1 000 000 000, which
 * nanosleep rejects with EINVAL — the sleep then returns immediately. */
TEST(sleep_longer_than_one_second)
{
    uint64_t elapsed = measure_sleep(1100 * NS_PER_MS);
    ASSERT_GE(elapsed, 1100 * NS_PER_MS);
    ASSERT_LT(elapsed, 1100 * NS_PER_MS + GENEROUS);
}

TEST(sleep_zero_returns_promptly)
{
    ASSERT_LT(measure_sleep(0), GENEROUS);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc time");
}
