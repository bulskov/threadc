/* threadc — Win32 backend (SRWLOCK, CONDITION_VARIABLE, INIT_ONCE).
 *
 */

#define WIN32_LEAN_AND_MEAN

#define NS_PER_S UINT64_C(1000000000)

#include "threadc/threadc.h"

#include <windows.h>

uint64_t tc_time_now_ns(void)
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remaining_ticks = ticks % ticks_per_second;

    return seconds * NS_PER_S + remaining_ticks * NS_PER_S / ticks_per_second;
}

void tc_sleep_ns(uint64_t ns)
{
    const uint64_t ns_per_ms = UINT64_C(1000000);
    uint64_t milliseconds = ns / ns_per_ms + (ns % ns_per_ms != 0);

    while (milliseconds != 0)
    {
        DWORD chunk =
            milliseconds >= MAXDWORD ? MAXDWORD - 1 : (DWORD)milliseconds;
        Sleep(chunk);
        milliseconds -= chunk;
    }
}
