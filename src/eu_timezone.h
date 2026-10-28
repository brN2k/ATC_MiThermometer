/*
 * Europe/Berlin civil display time for UTC clocks.
 * EU rule: last Sunday of March 01:00 UTC until last Sunday of October
 * 01:00 UTC. No RTC writes; only transform for user-facing display.
 * Rules assume current EU DST legislation; update firmware if rules change.
 */
#ifndef SYNC_V3_EU_TIMEZONE_H
#define SYNC_V3_EU_TIMEZONE_H

#include <stdint.h>

static inline int eu_tz_leap(uint32_t y) {
    return ((y % 4U) == 0U && (y % 100U != 0U || y % 400U == 0U));
}

static inline uint32_t eu_tz_last_sunday(uint32_t day_of_oct_or_mar_31) {
    /* 1970-01-01 was Thursday; Sunday is weekday 0. */
    return day_of_oct_or_mar_31 - ((day_of_oct_or_mar_31 + 4U) % 7U);
}

static inline int eu_tz_is_dst(uint32_t utc) {
    uint32_t day = utc / 86400U;
    uint32_t jan1 = 0;
    uint32_t year = 1970;
    /* Date conversion costs a few dozen integer operations when display
     * time is rendered. No periodic wakeup or DST-scan is necessary. */
    while (year < 2106U) {
        uint32_t next = jan1 + (eu_tz_leap(year) ? 366U : 365U);
        if (day < next)
            break;
        jan1 = next;
        year++;
    }
    const uint32_t leap = eu_tz_leap(year) ? 1U : 0U;
    const uint32_t march_last_sunday = eu_tz_last_sunday(jan1 + 89U + leap);
    const uint32_t october_last_sunday = eu_tz_last_sunday(jan1 + 303U + leap);
    const uint32_t start = march_last_sunday * 86400U + 3600U;
    const uint32_t end = october_last_sunday * 86400U + 3600U;
    return utc >= start && utc < end;
}

static inline uint32_t eu_tz_local_epoch(uint32_t utc) {
    return utc + (eu_tz_is_dst(utc) ? 7200U : 3600U);
}

#endif /* SYNC_V3_EU_TIMEZONE_H */
