/* Step 6: thread names.
 *
 *   tc_thread_start(&t, fn, arg, STRING_LIT("scanner"));
 *
 * The name is copied at start (the caller's buffer may go away), set in the
 * new thread before fn runs, and is what debuggers, perf, htop and Task
 * Manager show.  At most 15 bytes are kept on every platform, cut at a
 * whole UTF-8 character.  An empty name keeps the platform default.
 *
 * Each worker reads its own name back from the OS and records it; the main
 * thread checks. */

/* macOS: pthread_getname_np is an Apple extension. */
#define _DARWIN_C_SOURCE

#include "ctt.h"
#include "threadc/threadc.h"

#include <stdbool.h>
#include <string.h>

#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* On the supported platforms the name MUST be readable back — otherwise the
 * tests below would pass without checking anything. */
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define SKIP_IF_UNSUPPORTED(supported) ASSERT_TRUE(supported)
#else
#define SKIP_IF_UNSUPPORTED(supported)                                         \
    if (!(supported))                                                          \
    return
#endif

/* The calling thread's name as the OS reports it, as UTF-8.  Returns false
 * where the platform has no way to read it back. */
static bool current_thread_name(char *buf, size_t cap)
{
    buf[0] = '\0';
#if defined(__linux__)
    char tmp[16] = {0}; /* PR_GET_NAME always writes 16 bytes */
    if (prctl(PR_GET_NAME, tmp, 0, 0, 0) != 0)
        return false;
    strncpy(buf, tmp, cap - 1);
    buf[cap - 1] = '\0';
    return true;
#elif defined(__APPLE__)
    return pthread_getname_np(pthread_self(), buf, cap) == 0;
#elif defined(_WIN32)
    PWSTR wide = NULL;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &wide)))
        return false;
    int n =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, (int)cap, NULL, NULL);
    LocalFree(wide);
    return n > 0;
#else
    (void)cap;
    return false;
#endif
}

typedef struct
{
    bool supported;
    char name[64];
} seen_t;

static void record_name(void *arg)
{
    seen_t *s = arg;
    s->supported = current_thread_name(s->name, sizeof s->name);
}

/* Start a thread with `name`, return what it saw. */
static seen_t run_named(string_t name)
{
    seen_t s = {0};
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, record_name, &s, name).kind);
    tc_thread_join(&t);
    return s;
}

/* --- tests ------------------------------------------------------------- */

TEST(name_is_visible_inside_the_thread)
{
    seen_t s = run_named(STRING_LIT("scanner-1"));
    SKIP_IF_UNSUPPORTED(s.supported);
    ASSERT_STR_EQ("scanner-1", s.name);
}

TEST(exactly_fifteen_bytes_are_kept)
{
    seen_t s = run_named(STRING_LIT("123456789012345"));
    SKIP_IF_UNSUPPORTED(s.supported);
    ASSERT_STR_EQ("123456789012345", s.name);
}

TEST(long_name_is_cut_to_fifteen_bytes)
{
    seen_t s =
        run_named(STRING_LIT("a-thread-name-much-longer-than-fifteen-bytes"));
    SKIP_IF_UNSUPPORTED(s.supported);
    ASSERT_STR_EQ("a-thread-name-m", s.name);
}

/* "å" is two bytes in UTF-8.  Nine of them are 18 bytes; cutting at 15
 * would split the eighth, so only seven (14 bytes) are kept. */
TEST(cut_never_splits_a_utf8_character)
{
    seen_t s = run_named(STRING_LIT(
        "\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5"
        "\xc3\xa5"));
    SKIP_IF_UNSUPPORTED(s.supported);
    ASSERT_STR_EQ(
        "\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5\xc3\xa5", s.name);
}

/* The name is copied at start: overwriting the caller's buffer right after
 * tc_thread_start must not change what the thread is called. */
TEST(name_is_copied_at_start)
{
    char buf[] = "temporary";
    string_t name = {buf, sizeof buf - 1};
    seen_t s = {0};
    tc_thread_t t;
    ASSERT_EQ(TC_OK, tc_thread_start(&t, record_name, &s, name).kind);
    memset(buf, 'x', sizeof buf - 1);
    tc_thread_join(&t);
    SKIP_IF_UNSUPPORTED(s.supported);
    ASSERT_STR_EQ("temporary", s.name);
}

/* An empty name changes nothing: the thread keeps whatever the platform
 * gives a new thread (on Linux, the creating thread's name). */
TEST(empty_name_keeps_the_default)
{
    char main_name[64];
    seen_t plain = run_named(STRING_LIT(""));
    SKIP_IF_UNSUPPORTED(plain.supported);
    SKIP_IF_UNSUPPORTED(current_thread_name(main_name, sizeof main_name));
#if defined(__linux__)
    ASSERT_STR_EQ(main_name, plain.name);
#endif
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc name");
}
