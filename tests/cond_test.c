/* Step 4: condition variables.
 *
 *   void          tc_cond_init(tc_cond_t *c);
 *   void          tc_cond_destroy(tc_cond_t *c);
 *   void          tc_cond_wait(tc_cond_t *c, tc_mutex_t *m);
 *   tc_err_kind_t tc_cond_wait_timeout(tc_cond_t *c, tc_mutex_t *m,
 *                                      uint64_t timeout_ns);
 *   void          tc_cond_signal(tc_cond_t *c);
 *   void          tc_cond_broadcast(tc_cond_t *c);
 *
 * The contract, which every test relies on:
 *   - wait is called with the mutex held; it releases it while sleeping and
 *     holds it again when it returns — whatever the reason for returning.
 *   - wakeups can be spurious, so callers ALWAYS loop on a predicate (see
 *     wait_for_ready below — that is the pattern to copy).
 *   - wait_timeout returns TC_TIMEOUT once timeout_ns (relative, from now)
 *     has passed, TC_OK when woken (or spuriously) before that.
 *   - a timeout so large that now + timeout overflows means "no limit".
 *
 * A broken condition variable typically makes a thread wait forever.  The
 * tests therefore wait with generous timeouts where they can, and ctest
 * kills anything still running after 60 s. */

#include "ctt.h"
#include "threadc/threadc.h"

#include <stdint.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_S UINT64_C(1000000000)

/* "Long enough that only a bug gets there" — a correct test never waits
 * this long. */
#define PATIENCE (10 * NS_PER_S)

/* --- a gate: a flag, its mutex, the cond that announces it -------------- */

typedef struct
{
    tc_mutex_t mutex;
    tc_cond_t cond;
    bool ready;     /* the predicate                         */
    int waiting;    /* threads that are inside wait          */
    int woken;      /* threads that saw ready == true        */
    int timed_out;  /* threads that gave up                  */
    uint64_t delay; /* signal_after: how long to wait first  */
} gate_t;

static void gate_init(gate_t *g)
{
    *g = (gate_t){0};
    tc_mutex_init(&g->mutex);
    tc_cond_init(&g->cond);
}

static void gate_destroy(gate_t *g)
{
    tc_cond_destroy(&g->cond);
    tc_mutex_destroy(&g->mutex);
}

/* THE PATTERN.  Caller holds g->mutex.  Waits until g->ready, for at most
 * timeout_ns in total, looping over spurious wakeups and recomputing the
 * time that is left.  Returns with the mutex held. */
static tc_err_kind_t wait_for_ready(gate_t *g, uint64_t timeout_ns)
{
    uint64_t deadline = tc_time_now_ns() + timeout_ns;
    while (!g->ready)
    {
        uint64_t now = tc_time_now_ns();
        if (now >= deadline)
        {
            return TC_TIMEOUT;
        }
        tc_cond_wait_timeout(&g->cond, &g->mutex, deadline - now);
    }
    return TC_OK;
}

/* Worker: sleep g->delay, then set ready and signal — the other side of
 * the pattern: change the state under the mutex, then signal. */
static void signal_after(void *arg)
{
    gate_t *g = arg;
    tc_sleep_ns(g->delay);
    tc_mutex_lock(&g->mutex);
    g->ready = true;
    tc_cond_signal(&g->cond);
    tc_mutex_unlock(&g->mutex);
}

/* Worker: wait for ready with plain tc_cond_wait (no timeout). */
static void wait_plain(void *arg)
{
    gate_t *g = arg;
    tc_mutex_lock(&g->mutex);
    g->waiting++;
    while (!g->ready)
    {
        tc_cond_wait(&g->cond, &g->mutex);
    }
    g->woken++;
    tc_mutex_unlock(&g->mutex);
}

/* Worker: same, with a generous timeout so a lost wakeup is recorded
 * instead of hanging the test. */
static void wait_patient(void *arg)
{
    gate_t *g = arg;
    tc_mutex_lock(&g->mutex);
    g->waiting++;
    if (wait_for_ready(g, PATIENCE) == TC_OK)
    {
        g->woken++;
    }
    else
    {
        g->timed_out++;
    }
    tc_mutex_unlock(&g->mutex);
}

/* Main thread: wait until n workers are inside their wait.  A worker
 * increments `waiting` and enters wait without releasing the mutex in
 * between, so once main sees the count (under the mutex) they are all
 * really waiting. */
static void wait_until_waiting(gate_t *g, int n)
{
    for (;;)
    {
        tc_mutex_lock(&g->mutex);
        int w = g->waiting;
        tc_mutex_unlock(&g->mutex);
        if (w >= n)
        {
            return;
        }
        tc_thread_yield();
    }
}

/* --- basics ------------------------------------------------------------- */

TEST(init_destroy_many_times)
{
    tc_cond_t c;
    for (int i = 0; i < 1000; ++i)
    {
        tc_cond_init(&c);
        tc_cond_destroy(&c);
    }
}

/* Signalling with nobody waiting is allowed and does nothing (the signal is
 * not remembered — that is why there is always a predicate). */
TEST(signal_and_broadcast_without_waiters)
{
    gate_t g;
    gate_init(&g);
    tc_cond_signal(&g.cond);
    tc_cond_broadcast(&g.cond);
    gate_destroy(&g);
}

/* --- wait and signal ---------------------------------------------------- */

TEST(wait_returns_after_signal)
{
    gate_t g;
    gate_init(&g);
    tc_thread_t waiter;
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&waiter, wait_plain, &g, STRING_LIT("waiter")).kind);

    wait_until_waiting(&g, 1);
    tc_mutex_lock(&g.mutex);
    g.ready = true;
    tc_cond_signal(&g.cond);
    tc_mutex_unlock(&g.mutex);

    tc_thread_join(&waiter);
    ASSERT_EQ(1, g.woken);
    gate_destroy(&g);
}

/* The signal comes BEFORE anybody waits.  The waiter must still finish,
 * because it checks the predicate before waiting — a signal alone would be
 * lost. */
TEST(predicate_set_before_wait_is_not_lost)
{
    gate_t g;
    gate_init(&g);
    tc_mutex_lock(&g.mutex);
    g.ready = true;
    tc_cond_signal(&g.cond); /* nobody listening yet */
    tc_mutex_unlock(&g.mutex);

    tc_thread_t waiter;
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&waiter, wait_plain, &g, STRING_LIT("late")).kind);
    tc_thread_join(&waiter);
    ASSERT_EQ(1, g.woken);
    gate_destroy(&g);
}

/* --- broadcast ---------------------------------------------------------- */

#define WAITERS 8

TEST(broadcast_wakes_every_waiter)
{
    gate_t g;
    gate_init(&g);
    tc_thread_t waiters[WAITERS];
    for (int i = 0; i < WAITERS; ++i)
    {
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&waiters[i], wait_patient, &g, STRING_LIT("bc"))
                .kind);
    }

    wait_until_waiting(&g, WAITERS);
    tc_mutex_lock(&g.mutex);
    g.ready = true;
    tc_cond_broadcast(&g.cond);
    tc_mutex_unlock(&g.mutex);

    for (int i = 0; i < WAITERS; ++i)
    {
        tc_thread_join(&waiters[i]);
    }
    ASSERT_EQ(WAITERS, g.woken);
    ASSERT_EQ(0, g.timed_out);
    gate_destroy(&g);
}

/* --- wait_timeout ------------------------------------------------------- */

/* Nobody signals: one call must sleep at least the timeout, then report
 * TC_TIMEOUT.  Catches the classic bug of handing a RELATIVE time to an
 * API that expects an ABSOLUTE deadline: the deadline is then in the past
 * and the call returns immediately. */
TEST(wait_timeout_times_out_after_at_least_the_timeout)
{
    gate_t g;
    gate_init(&g);
    tc_mutex_lock(&g.mutex);
    uint64_t start = tc_time_now_ns();
    tc_err_kind_t r = tc_cond_wait_timeout(&g.cond, &g.mutex, 50 * NS_PER_MS);
    uint64_t elapsed = tc_time_now_ns() - start;
    tc_mutex_unlock(&g.mutex); /* debug builds assert if wait did not re-lock */

    ASSERT_EQ(TC_TIMEOUT, r);
    ASSERT_GE(elapsed, 50 * NS_PER_MS);
    ASSERT_LT(elapsed, 50 * NS_PER_MS + NS_PER_S);
    gate_destroy(&g);
}

TEST(wait_timeout_zero_times_out_immediately)
{
    gate_t g;
    gate_init(&g);
    tc_mutex_lock(&g.mutex);
    uint64_t start = tc_time_now_ns();
    tc_err_kind_t r = tc_cond_wait_timeout(&g.cond, &g.mutex, 0);
    uint64_t elapsed = tc_time_now_ns() - start;
    tc_mutex_unlock(&g.mutex);

    ASSERT_EQ(TC_TIMEOUT, r);
    ASSERT_LT(elapsed, NS_PER_S);
    gate_destroy(&g);
}

/* Signalled after 20 ms, long before the timeout: TC_OK, and it really did
 * wait for the signal.  This also shows the mutex is released while
 * waiting — otherwise signal_after could never lock it to set ready. */
TEST(wait_timeout_returns_ok_when_signalled)
{
    gate_t g;
    gate_init(&g);
    g.delay = 20 * NS_PER_MS;
    tc_thread_t signaller;

    tc_mutex_lock(&g.mutex);
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&signaller, signal_after, &g, STRING_LIT("sig")).kind);
    tc_err_kind_t r = wait_for_ready(&g, PATIENCE);
    uint64_t elapsed = tc_time_now_ns() - start;
    tc_mutex_unlock(&g.mutex);
    tc_thread_join(&signaller);

    ASSERT_EQ(TC_OK, r);
    ASSERT_GE(elapsed, 20 * NS_PER_MS);
    ASSERT_LT(elapsed, PATIENCE);
    gate_destroy(&g);
}

/* A timeout above one second: the deadline's nanosecond field must be
 * normalised (tv_nsec < 1 000 000 000), or pthreads rejects it with EINVAL
 * at once. */
TEST(wait_timeout_longer_than_one_second)
{
    gate_t g;
    gate_init(&g);
    g.delay = 50 * NS_PER_MS;
    tc_thread_t signaller;

    tc_mutex_lock(&g.mutex);
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&signaller, signal_after, &g, STRING_LIT("sig")).kind);
    tc_err_kind_t r = TC_OK;
    while (!g.ready && r == TC_OK)
    {
        r = tc_cond_wait_timeout(&g.cond, &g.mutex, 1500 * NS_PER_MS);
    }
    uint64_t elapsed = tc_time_now_ns() - start;
    tc_mutex_unlock(&g.mutex);
    tc_thread_join(&signaller);

    ASSERT_EQ(TC_OK, r);
    ASSERT_GE(elapsed, 50 * NS_PER_MS);
    gate_destroy(&g);
}

/* UINT64_MAX means "no limit": now + timeout must not overflow into a
 * deadline in the past (which would time out at once). */
TEST(huge_timeout_does_not_overflow)
{
    gate_t g;
    gate_init(&g);
    g.delay = 50 * NS_PER_MS;
    tc_thread_t signaller;

    tc_mutex_lock(&g.mutex);
    uint64_t start = tc_time_now_ns();
    ASSERT_EQ(
        TC_OK,
        tc_thread_start(&signaller, signal_after, &g, STRING_LIT("sig")).kind);
    tc_err_kind_t r = TC_OK;
    while (!g.ready && r == TC_OK)
    {
        r = tc_cond_wait_timeout(&g.cond, &g.mutex, UINT64_MAX);
    }
    uint64_t elapsed = tc_time_now_ns() - start;
    tc_mutex_unlock(&g.mutex);
    tc_thread_join(&signaller);

    ASSERT_EQ(TC_OK, r);
    ASSERT_GE(elapsed, 50 * NS_PER_MS);
    gate_destroy(&g);
}

/* --- producer / consumer ------------------------------------------------ */
/* The classic: a small bounded queue, two conditions (not_empty, not_full)
 * sharing one mutex.  Producers block when it is full, consumers when it is
 * empty. */

#define CAPACITY 4

typedef struct
{
    tc_mutex_t mutex;
    tc_cond_t not_empty;
    tc_cond_t not_full;
    long long items[CAPACITY];
    int head;  /* next item to take */
    int count; /* items in the queue */
} queue_t;

static void queue_init(queue_t *q)
{
    *q = (queue_t){0};
    tc_mutex_init(&q->mutex);
    tc_cond_init(&q->not_empty);
    tc_cond_init(&q->not_full);
}

static void queue_destroy(queue_t *q)
{
    tc_cond_destroy(&q->not_full);
    tc_cond_destroy(&q->not_empty);
    tc_mutex_destroy(&q->mutex);
}

static void queue_put(queue_t *q, long long item)
{
    tc_mutex_lock(&q->mutex);
    while (q->count == CAPACITY)
    {
        tc_cond_wait(&q->not_full, &q->mutex);
    }
    q->items[(q->head + q->count) % CAPACITY] = item;
    q->count++;
    tc_cond_signal(&q->not_empty);
    tc_mutex_unlock(&q->mutex);
}

static long long queue_take(queue_t *q)
{
    tc_mutex_lock(&q->mutex);
    while (q->count == 0)
    {
        tc_cond_wait(&q->not_empty, &q->mutex);
    }
    long long item = q->items[q->head];
    q->head = (q->head + 1) % CAPACITY;
    q->count--;
    tc_cond_signal(&q->not_full);
    tc_mutex_unlock(&q->mutex);
    return item;
}

#define ITEMS 10000

typedef struct
{
    queue_t *queue;
    long long first; /* producer: values first .. first + count - 1 */
    int count;       /* items to put or take                        */
    long long sum;   /* consumer: sum of everything taken           */
    bool in_order;   /* consumer: every item larger than the last   */
} worker_t;

static void producer(void *arg)
{
    worker_t *w = arg;
    for (int i = 0; i < w->count; ++i)
    {
        queue_put(w->queue, w->first + i);
    }
}

static void consumer(void *arg)
{
    worker_t *w = arg;
    long long last = 0;
    w->in_order = true;
    for (int i = 0; i < w->count; ++i)
    {
        long long item = queue_take(w->queue);
        if (item <= last)
        {
            w->in_order = false;
        }
        last = item;
        w->sum += item;
    }
}

/* One producer, one consumer: every item arrives, in order. */
TEST(producer_consumer_one_to_one)
{
    queue_t q;
    queue_init(&q);
    worker_t p = {.queue = &q, .first = 1, .count = ITEMS};
    worker_t c = {.queue = &q, .count = ITEMS};
    tc_thread_t tp, tcn;
    ASSERT_EQ(
        TC_OK, tc_thread_start(&tcn, consumer, &c, STRING_LIT("cons")).kind);
    ASSERT_EQ(
        TC_OK, tc_thread_start(&tp, producer, &p, STRING_LIT("prod")).kind);
    tc_thread_join(&tp);
    tc_thread_join(&tcn);
    queue_destroy(&q);

    ASSERT_TRUE(c.in_order);
    ASSERT_EQ((long long)ITEMS * (ITEMS + 1) / 2, c.sum);
}

/* Four producers, four consumers on one queue: nothing lost, nothing
 * duplicated.  Values 1..N in total, so the sum must be N(N+1)/2. */
#define PRODUCERS 4
#define CONSUMERS 4

TEST(producer_consumer_many_to_many)
{
    queue_t q;
    queue_init(&q);
    worker_t prods[PRODUCERS], conss[CONSUMERS];
    tc_thread_t tp[PRODUCERS], tcn[CONSUMERS];

    for (int i = 0; i < CONSUMERS; ++i)
    {
        conss[i] =
            (worker_t){.queue = &q, .count = ITEMS * PRODUCERS / CONSUMERS};
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&tcn[i], consumer, &conss[i], STRING_LIT("cons"))
                .kind);
    }
    for (int i = 0; i < PRODUCERS; ++i)
    {
        prods[i] = (worker_t){
            .queue = &q, .first = 1 + (long long)i * ITEMS, .count = ITEMS};
        ASSERT_EQ(
            TC_OK,
            tc_thread_start(&tp[i], producer, &prods[i], STRING_LIT("prod"))
                .kind);
    }
    for (int i = 0; i < PRODUCERS; ++i)
    {
        tc_thread_join(&tp[i]);
    }
    for (int i = 0; i < CONSUMERS; ++i)
    {
        tc_thread_join(&tcn[i]);
    }
    queue_destroy(&q);

    long long total = 0;
    for (int i = 0; i < CONSUMERS; ++i)
    {
        total += conss[i].sum;
    }
    long long n = (long long)ITEMS * PRODUCERS;
    ASSERT_EQ(n * (n + 1) / 2, total);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc cond");
}
