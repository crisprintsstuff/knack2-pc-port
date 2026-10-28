// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <intrin.h>
#include <stdio.h>
#include <string.h>
typedef struct { int64_t seconds, nanoseconds; } TimeSpec;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static uint64_t tsc_frequency, initial_tsc;
static uint64_t tick(void) { _mm_lfence(); uint64_t t = __rdtsc(); _mm_lfence(); return t; }
static BOOL CALLBACK initialize(PINIT_ONCE o, void *p, void **c) {
    (void)o; (void)p; (void)c;
    LARGE_INTEGER qfreq, begin, end; QueryPerformanceFrequency(&qfreq); QueryPerformanceCounter(&begin);
    uint64_t first = tick(); Sleep(40); uint64_t last = tick(); QueryPerformanceCounter(&end);
    tsc_frequency = (uint64_t)((double)(last-first) * (double)qfreq.QuadPart / (double)(end.QuadPart-begin.QuadPart));
    initial_tsc = tick();
    printf("Calibrated invariant TSC: %llu Hz\n", (unsigned long long)tsc_frequency); return TRUE;
}
static uint64_t GUEST_ABI frequency(void) { InitOnceExecuteOnce(&once,initialize,NULL,NULL); return tsc_frequency; }
static uint64_t GUEST_ABI process_time(void) {
    uint64_t f = frequency(), d = tick()-initial_tsc;
    return (d/f)*1000000 + ((d%f)*1000000)/f;
}
uint64_t runtime_tsc_frequency(void){return frequency();}
uint64_t runtime_process_time_us(void){return process_time();}
static int32_t GUEST_ABI clock_gettime(uint32_t id, TimeSpec *out) {
    if (!out) return (int32_t)UINT32_C(0x80020016);
    if (id == 0 || id == 9 || id == 10 || id == 13) {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
        uint64_t n = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - UINT64_C(116444736000000000);
        out->seconds = (int64_t)(n/10000000); out->nanoseconds = (int64_t)((n%10000000)*100);
        if(id==13)out->nanoseconds=0;
        return 0;
    }
    if (id == 4 || id == 5 || id == 7 || id == 8 || id == 11 || id == 12) {
        LARGE_INTEGER counter,f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&counter);
        out->seconds = counter.QuadPart/f.QuadPart;
        out->nanoseconds = (counter.QuadPart%f.QuadPart)*1000000000/f.QuadPart; return 0;
    }
    return (int32_t)UINT32_C(0x80020016);
}
static int GUEST_ABI posix_clock_gettime(uint32_t id,TimeSpec *out){int32_t r=clock_gettime(id,out);if(r){*runtime_errno()=r&65535;return -1;}return 0;}
static int32_t GUEST_ABI usleep(uint32_t us) {
    if (!us) { SwitchToThread(); return 0; }
    HANDLE timer = CreateWaitableTimerExW(NULL,NULL,0x2,TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL,TRUE,NULL);
    if (!timer) return (int32_t)UINT32_C(0x8002000c);
    LARGE_INTEGER due; due.QuadPart = -(int64_t)us*10;
    BOOL set = SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE);
    DWORD result = set ? WaitForSingleObject(timer,INFINITE) : WAIT_FAILED;
    CloseHandle(timer); return result == WAIT_OBJECT_0 ? 0 : (int32_t)UINT32_C(0x80020016);
}
// Process time counter: TSC ticks since start, at the TSC frequency.
static uint64_t GUEST_ABI process_counter(void) { frequency(); return tick() - initial_tsc; }
typedef struct { int64_t seconds, microseconds; } TimeVal;
static void wall_clock(TimeVal *out) {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t n = ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - UINT64_C(116444736000000000)) / 10;
    out->seconds = (int64_t)(n / 1000000); out->microseconds = (int64_t)(n % 1000000);
}
static int32_t GUEST_ABI kernel_gettimeofday(TimeVal *out) { if (!out) return (int32_t)UINT32_C(0x80020016); wall_clock(out); return 0; }
// struct timezone: minutes west of UTC, then the daylight-saving flag (as minutes here).
typedef struct { int32_t minutes_west, dst_minutes; } TimeZone;
static void zone(int32_t *east_seconds, int32_t *dst_seconds) {
    TIME_ZONE_INFORMATION info; DWORD state = GetTimeZoneInformation(&info);
    *east_seconds = -(int32_t)info.Bias * 60;
    *dst_seconds = state == TIME_ZONE_ID_DAYLIGHT ? -(int32_t)info.DaylightBias * 60 : 0;
}
static int GUEST_ABI posix_gettimeofday(TimeVal *out, TimeZone *tz) {
    if (out) wall_clock(out);
    if (tz) { int32_t east, dst; zone(&east, &dst); tz->minutes_west = -east / 60; tz->dst_minutes = dst / 60; }
    return 0;
}
static int32_t sleep_spec(const TimeSpec *request, TimeSpec *remaining) {
    if (!request || request->seconds < 0 || request->nanoseconds < 0 || request->nanoseconds >= 1000000000) return 22;
    uint64_t us = (uint64_t)request->seconds * 1000000 + (uint64_t)(request->nanoseconds + 999) / 1000;
    while (us) { uint32_t part = us > 1000000000 ? 1000000000 : (uint32_t)us; usleep(part); us -= part; }
    if (remaining) { remaining->seconds = 0; remaining->nanoseconds = 0; }
    return 0;
}
static int GUEST_ABI posix_nanosleep(const TimeSpec *request, TimeSpec *remaining) {
    int e = sleep_spec(request, remaining); if (e) { *runtime_errno() = e; return -1; } return 0;
}
static int GUEST_ABI posix_usleep(uint32_t us) { return usleep(us) ? (*runtime_errno() = 22, -1) : 0; }
// Local = UTC + the zone's offset east + the daylight-saving offset (current rules, as on Windows
// GetTimeZoneInformation). OrbisTimesec is { time_t t; u32 offset east; u32 dst }.
typedef struct { int64_t t; uint32_t east_seconds, dst_seconds; } TimeSec;
static int32_t GUEST_ABI utc_to_local(int64_t utc, int64_t *local, TimeSec *info, uint64_t *dst) {
    if (!local) return (int32_t)UINT32_C(0x80020016);
    int32_t east, saving; zone(&east, &saving);
    *local = utc + east + saving;
    if (info) { info->t = utc; info->east_seconds = (uint32_t)east; info->dst_seconds = (uint32_t)saving; }
    if (dst) *dst = (uint64_t)saving;
    return 0;
}
static int32_t GUEST_ABI local_to_utc(int64_t local, int64_t unused, int64_t *utc, TimeZone *tz, int32_t *dst) {
    (void)unused;
    int32_t east, saving; zone(&east, &saving);
    if (utc) *utc = local - east - saving;
    if (tz) { tz->minutes_west = -east / 60; tz->dst_minutes = saving / 60; }
    if (dst) *dst = saving;
    return 0;
}
uintptr_t time_resolve(const char *identity) {
    if (!strstr(identity,"#libkernel#1#libkernel#257")&&!strstr(identity,"#libScePosix#1#libkernel#257")) return 0;
    const char *api = runtime_api_name(identity);
    if (!strcmp(api,"sceKernelGetTscFrequency")) return (uintptr_t)frequency;
    if (!strcmp(api,"sceKernelGetProcessTime")) return (uintptr_t)process_time;
    if (!strcmp(api,"sceKernelClockGettime")) return (uintptr_t)clock_gettime;
    if (!strcmp(api,"sceKernelUsleep")) return (uintptr_t)usleep;
    if (!strcmp(api,"clock_gettime")||!strcmp(api,"_clock_gettime")) return (uintptr_t)posix_clock_gettime;
    if (!strcmp(api,"sceKernelGetProcessTimeCounter")) return (uintptr_t)process_counter;
    if (!strcmp(api,"sceKernelGetProcessTimeCounterFrequency")) return (uintptr_t)frequency;
    if (!strcmp(api,"sceKernelGettimeofday")) return (uintptr_t)kernel_gettimeofday;
    if (!strcmp(api,"gettimeofday")) return (uintptr_t)posix_gettimeofday;
    if (!strcmp(api,"nanosleep")||!strcmp(api,"_nanosleep")) return (uintptr_t)posix_nanosleep;
    if (!strcmp(api,"usleep")) return (uintptr_t)posix_usleep;
    if (!strcmp(api,"sceKernelConvertUtcToLocaltime")) return (uintptr_t)utc_to_local;
    if (!strcmp(api,"sceKernelConvertLocaltimeToUtc")) return (uintptr_t)local_to_utc;
    return 0;
}
