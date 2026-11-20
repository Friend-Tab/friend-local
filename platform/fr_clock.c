/* fr_clock.c — see fr_clock.h. */

#include "fr_clock.h"

#include <time.h>

uint64_t fr_now_unix_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* From the two breakdowns of the same instant rather than from a
 * tm_gmtoff, which is not in C99. */
int32_t fr_local_utc_offset_minutes(void)
{
    time_t    now = time(0);
    struct tm local;
    struct tm utc;
    int32_t   minutes;

    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
    minutes = (int32_t)((local.tm_hour - utc.tm_hour) * 60 +
                        (local.tm_min - utc.tm_min));

    /* The two readings can fall either side of midnight. */
    if (local.tm_yday != utc.tm_yday) {
        int32_t ahead = (local.tm_year > utc.tm_year ||
                         (local.tm_year == utc.tm_year &&
                          local.tm_yday > utc.tm_yday))
                            ? 1
                            : -1;
        minutes += ahead * 24 * 60;
    }
    return minutes;
}
