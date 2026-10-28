// SPDX-License-Identifier: GPL-2.0-or-later
// libSceRtc: ticks are microseconds since 0001-01-01 00:00:00 UTC (proleptic Gregorian).
#include "runtime.h"
#include <string.h>

#define ERR_POINTER ((int32_t)UINT32_C(0x80B50002))
#define ERR_VALUE ((int32_t)UINT32_C(0x80B50003))
#define DAY_US INT64_C(86400000000)
#define UNIX_EPOCH_TICKS INT64_C(62135596800000000) // 1970-01-01 as a tick
#define UNIX_EPOCH_DAYS INT64_C(719162)              // days from 0001-01-01 to 1970-01-01
typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } DateTime;
_Static_assert(sizeof(DateTime) == 16, "SceRtcDateTime layout");

// Days since 1970-01-01 and back (H. Hinnant's civil calendar algorithms).
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}
static int days_in_month(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : days[m - 1];
}
static uint64_t now_tick(void) {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t n = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - UINT64_C(116444736000000000);
    return (uint64_t)UNIX_EPOCH_TICKS + n / 10;
}
// Offset of local time from UTC in microseconds, with daylight saving (current rules).
static int64_t local_offset(void) {
    TIME_ZONE_INFORMATION info; DWORD state = GetTimeZoneInformation(&info);
    int64_t minutes = -(int64_t)info.Bias - (state == TIME_ZONE_ID_DAYLIGHT ? (int64_t)info.DaylightBias : 0);
    return minutes * 60000000;
}

static int32_t GUEST_ABI current_tick(uint64_t *tick) { if (!tick) return ERR_POINTER; *tick = now_tick(); return 0; }
static int32_t GUEST_ABI utc_to_local(const uint64_t *utc, uint64_t *local) {
    if (!utc || !local) return ERR_POINTER; *local = (uint64_t)((int64_t)*utc + local_offset()); return 0;
}
static int32_t GUEST_ABI local_to_utc(const uint64_t *local, uint64_t *utc) {
    if (!utc || !local) return ERR_POINTER; *utc = (uint64_t)((int64_t)*local - local_offset()); return 0;
}
static int32_t GUEST_ABI get_tick(const DateTime *t, uint64_t *tick) {
    if (!t || !tick) return ERR_POINTER;
    if (t->year < 1 || t->year > 9999 || t->month < 1 || t->month > 12 || t->day < 1 ||
        t->day > days_in_month(t->year, t->month) || t->hour > 23 || t->minute > 59 || t->second > 59 ||
        t->microsecond > 999999) return ERR_VALUE;
    int64_t days = days_from_civil(t->year, t->month, t->day) + UNIX_EPOCH_DAYS;
    *tick = (uint64_t)(days * DAY_US + ((int64_t)t->hour * 3600 + t->minute * 60 + t->second) * 1000000 + t->microsecond);
    return 0;
}
static int32_t GUEST_ABI set_tick(DateTime *t, const uint64_t *tick) {
    if (!t || !tick) return ERR_POINTER;
    int64_t days = (int64_t)(*tick / (uint64_t)DAY_US), rest = (int64_t)(*tick % (uint64_t)DAY_US);
    int64_t year; int month, day;
    civil_from_days(days - UNIX_EPOCH_DAYS, &year, &month, &day);
    if (year > 9999) return ERR_VALUE;
    t->year = (uint16_t)year; t->month = (uint16_t)month; t->day = (uint16_t)day;
    t->hour = (uint16_t)(rest / 3600000000); t->minute = (uint16_t)(rest / 60000000 % 60);
    t->second = (uint16_t)(rest / 1000000 % 60); t->microsecond = (uint32_t)(rest % 1000000);
    return 0;
}
static int32_t GUEST_ABI set_time_t(DateTime *t, int64_t seconds) {
    if (!t) return ERR_POINTER;
    if (seconds < 0) return ERR_VALUE;
    uint64_t tick = (uint64_t)UNIX_EPOCH_TICKS + (uint64_t)seconds * 1000000;
    return set_tick(t, &tick);
}
static int32_t GUEST_ABI add_days(uint64_t *out, const uint64_t *in, int32_t days) {
    if (!out || !in) return ERR_POINTER; *out = (uint64_t)((int64_t)*in + days * DAY_US); return 0;
}

uintptr_t rtc_resolve(const char *identity) {
    if (!strstr(identity, "#libSceRtc#1#libSceRtc#257")) return 0;
    const char *api = runtime_api_name(identity);
#define API(n, f) if (!strcmp(api, n)) return (uintptr_t)f
    API("sceRtcGetCurrentTick", current_tick); API("sceRtcGetCurrentNetworkTick", current_tick);
    API("sceRtcConvertUtcToLocalTime", utc_to_local); API("sceRtcConvertLocalTimeToUtc", local_to_utc);
    API("sceRtcGetTick", get_tick); API("sceRtcSetTick", set_tick); API("sceRtcSetTime_t", set_time_t);
    API("sceRtcTickAddDays", add_days);
#undef API
    return 0;
}
