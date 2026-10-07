/* threadc — pthreads backend (Linux; macOS).
 *
 */

#define _POSIX_C_SOURCE 200809L

#define NS_PER_S UINT64_C(1000000000)

#include "threadc/threadc.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

uint64_t tc_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

void tc_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long)(ns % NS_PER_S);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}
