// SPDX-License-Identifier: GPL-2.0-or-later
// Stand-in for PS4 libc imports when the dump has no libc.prx. CUSA08014 ships libc.prx, and
// the linker binds those imports to it; this file answers them only if that module is absent.
// Host CRT functions are wrapped for the SysV ABI and PS4 types: long is 64-bit, wchar_t is 16-bit
// (as on Windows), FILE is the host FILE opaque to the guest, struct tm has BSD's extra fields.
// The variadic functions are in libc_format.c and libc_varargs.S, the mspaces in libc_mspace.c.
#include "runtime.h"
#include "libc_format.h"
#include "libc_mspace.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void stop(const char *reason) {
    fprintf(stderr, "RUNTIME_ERROR: %s\n", reason);
    fflush(stderr);
    ExitProcess(23);
}
// PS4 errno values are FreeBSD's, equal to the host's up to ERANGE.
static void sync_errno(void) { *runtime_errno() = errno == EILSEQ ? 86 : errno; }

// ---- strings -------------------------------------------------------------------------------
static char *GUEST_ABI g_strchr(const char *s, int c) { return strchr(s, c); }
static char *GUEST_ABI g_strrchr(const char *s, int c) { return strrchr(s, c); }
static char *GUEST_ABI g_strstr(const char *s, const char *t) { return strstr(s, t); }
static char *GUEST_ABI g_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *GUEST_ABI g_strncpy(char *d, const char *s, uint64_t n) { return strncpy(d, s, (size_t)n); }
static char *GUEST_ABI g_strncat(char *d, const char *s, uint64_t n) { return strncat(d, s, (size_t)n); }
static uint64_t GUEST_ABI g_strspn(const char *s, const char *t) { return strspn(s, t); }
static uint64_t GUEST_ABI g_strcspn(const char *s, const char *t) { return strcspn(s, t); }
static int GUEST_ABI g_strcasecmp(const char *s, const char *t) { return _stricmp(s, t); }
static int GUEST_ABI g_strncasecmp(const char *s, const char *t, uint64_t n) { return _strnicmp(s, t, (size_t)n); }
static void *GUEST_ABI g_memchr(const void *p, int c, uint64_t n) { return memchr(p, c, (size_t)n); }
static uint64_t GUEST_ABI g_wcslen(const uint16_t *s) { const uint16_t *e = s; while (*e) ++e; return (uint64_t)(e - s); }
static float GUEST_ABI g_strtof(const char *s, char **end) { errno = 0; float v = strtof(s, end); sync_errno(); return v; }
static int64_t GUEST_ABI g_strtoll(const char *s, char **end, int base) { errno = 0; long long v = strtoll(s, end, base); sync_errno(); return v; }
static uint64_t GUEST_ABI g_strtoull(const char *s, char **end, int base) { errno = 0; unsigned long long v = strtoull(s, end, base); sync_errno(); return v; }
// Annex K: on failure the destination is cleared and a nonzero errno value returned.
static int GUEST_ABI g_memcpy_s(void *d, uint64_t size, const void *s, uint64_t n) {
    if (!d) return EINVAL;
    if (!s || n > size) { memset(d, 0, (size_t)size); return s ? ERANGE : EINVAL; }
    memmove(d, s, (size_t)n);
    return 0;
}
static int GUEST_ABI g_strncpy_s(char *d, uint64_t size, const char *s, uint64_t n) {
    if (!d || !size) return EINVAL;
    if (!s) { d[0] = 0; return EINVAL; }
    uint64_t length = strnlen(s, (size_t)(n < size ? n : size));
    if (length >= size) { d[0] = 0; return ERANGE; }
    memcpy(d, s, (size_t)length); d[length] = 0;
    return 0;
}
// UTF-16 to UTF-8; mbstate is not needed because surrogate pairs are consumed together.
static uint64_t GUEST_ABI g_wcsrtombs(char *d, const uint16_t **src, uint64_t n, void *state) {
    (void)state;
    const uint16_t *s = *src;
    uint64_t written = 0;
    for (;;) {
        uint32_t c = *s; int units = 1;
        if (c >= 0xd800 && c < 0xdc00 && s[1] >= 0xdc00 && s[1] < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (s[1] - 0xdc00); units = 2; }
        else if (c >= 0xd800 && c < 0xe000) { *runtime_errno() = 86; if (d) *src = s; return (uint64_t)-1; }
        char b[4]; int k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); k = 4; }
        if (d) {
            if (written + (uint64_t)k > n) { *src = s; return written; }
            memcpy(d + written, b, (size_t)k);
        }
        if (!c) { if (d) *src = NULL; return written; }
        written += (uint64_t)k; s += units;
    }
}

// ---- ctype (Dinkumware tables: index -1 is EOF) -------------------------------------------------
enum { XD = 0x01, UP = 0x02, SP = 0x04, PU = 0x08, LO = 0x10, DI = 0x20, CN = 0x40, BB = 0x80 };
static int16_t ctype_table[257], lower_table[257], upper_table[257];
static INIT_ONCE ctype_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK build_ctype(PINIT_ONCE once, void *p, void **c) {
    (void)once; (void)p; (void)c;
    ctype_table[0] = 0; lower_table[0] = upper_table[0] = -1;
    for (int ch = 0; ch < 256; ++ch) {
        int16_t v = 0;
        if (ch >= '0' && ch <= '9') v = DI | XD;
        else if (ch >= 'A' && ch <= 'Z') v = UP | (ch <= 'F' ? XD : 0);
        else if (ch >= 'a' && ch <= 'z') v = LO | (ch <= 'f' ? XD : 0);
        else if (ch == ' ') v = SP;
        else if (ch >= '\t' && ch <= '\r') v = CN;
        else if (ch < 0x20 || ch == 0x7f) v = BB;
        else if (ch < 0x7f) v = PU;
        ctype_table[ch + 1] = v;
        lower_table[ch + 1] = (int16_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        upper_table[ch + 1] = (int16_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    }
    return TRUE;
}
static const int16_t *GUEST_ABI g_getpctype(void) { InitOnceExecuteOnce(&ctype_once, build_ctype, NULL, NULL); return ctype_table + 1; }
static const int16_t *GUEST_ABI g_getptolower(void) { InitOnceExecuteOnce(&ctype_once, build_ctype, NULL, NULL); return lower_table + 1; }
static const int16_t *GUEST_ABI g_getptoupper(void) { InitOnceExecuteOnce(&ctype_once, build_ctype, NULL, NULL); return upper_table + 1; }

// ---- math ----------------------------------------------------------------------------------------
#define D1(f) static double GUEST_ABI g_##f(double x) { return f(x); }
#define D2(f) static double GUEST_ABI g_##f(double x, double y) { return f(x, y); }
#define F1(f) static float GUEST_ABI g_##f(float x) { return f(x); }
#define F2(f) static float GUEST_ABI g_##f(float x, float y) { return f(x, y); }
D1(cos) D1(sin) D1(tan) D1(exp) D1(exp2) D1(log) D1(log10) D2(atan2) D2(hypot) D2(pow)
F1(acosf) F1(asinf) F1(asinhf) F1(atanf) F1(cbrtf) F1(cosf) F1(erff) F1(exp2f) F1(expf)
F1(log10f) F1(log2f) F1(logf) F1(roundf) F1(sinf) F1(tanf) F1(tanhf)
F2(atan2f) F2(fmodf) F2(nextafterf) F2(powf)
static float GUEST_ABI g_frexpf(float x, int *e) { return frexpf(x, e); }
static float GUEST_ABI g_ldexpf(float x, int e) { return ldexpf(x, e); }
static float GUEST_ABI g_modff(float x, float *i) { return modff(x, i); }
static void GUEST_ABI g_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static void GUEST_ABI g_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static int GUEST_ABI g_isfinite(double x) { return isfinite(x); }
static int GUEST_ABI g_isfinitef(float x) { return isfinite(x); }
// Dinkumware helpers behind coshf/sinhf: y * cosh(x), y * sinh(x).
static float GUEST_ABI g_fcosh(float x, float y) { return y * coshf(x); }
static float GUEST_ABI g_fsinh(float x, float y) { return y * sinhf(x); }
// compiler-rt builtins the eboot calls.
static double GUEST_ABI g_powidf2(double a, int b) {
    int reciprocal = b < 0; double r = 1;
    for (;;) { if (b & 1) r *= a; b /= 2; if (!b) break; a *= a; }
    return reciprocal ? 1 / r : r;
}
static float GUEST_ABI g_powisf2(float a, int b) { return (float)g_powidf2(a, b); }
// (high:low) / divisor with high < divisor, so the quotient fits: one DIV instruction. A plain
// 128-bit division would call compiler-rt's __udivti3, which is not linked.
static uint64_t divide_128_by_64(uint64_t high, uint64_t low, uint64_t divisor) {
    uint64_t quotient, remainder;
    __asm__("divq %4" : "=a"(quotient), "=d"(remainder) : "a"(low), "d"(high), "rm"(divisor));
    (void)remainder;
    return quotient;
}
static unsigned __int128 GUEST_ABI g_udivti3(unsigned __int128 n, unsigned __int128 d) {
    if (!(d >> 64)) {
        uint64_t divisor = (uint64_t)d, high = (uint64_t)(n >> 64);
        uint64_t low = divide_128_by_64(high % divisor, (uint64_t)n, divisor);
        return ((unsigned __int128)(high / divisor) << 64) | low;
    }
    unsigned __int128 q = 0, r = 0;
    for (int i = 127; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (unsigned __int128)1 << i; }
    }
    return q;
}

// ---- stdio ---------------------------------------------------------------------------------------
// Data is staged through host memory: ReadFile/WriteFile must not touch guest pages that the
// GPU layer write-protects, while a user-mode memcpy fault there is handled.
enum { STAGE = 64 * 1024 };
static FILE *GUEST_ABI g_fopen(const char *path, const char *mode) {
    char host[2048], host_mode[8]; size_t k = 0;
    int writing = strpbrk(mode, "wa+") != NULL;
    int e = runtime_file_translate(path, host, sizeof(host), writing);
    if (e) { *runtime_errno() = e; return NULL; }
    for (const char *m = mode; *m && k < 5; ++m) if (*m != 'b' && *m != 't') host_mode[k++] = *m;
    host_mode[k++] = 'b'; host_mode[k] = 0; // never text mode
    FILE *f = fopen(host, host_mode);
    if (!f) sync_errno();
    return f;
}
static int GUEST_ABI g_fclose(FILE *f) { return fclose(f); }
static uint64_t GUEST_ABI g_fread(void *p, uint64_t size, uint64_t n, FILE *f) {
    if (!size || !n) return 0;
    uint64_t total = size * n, done = 0;
    char *stage = malloc(STAGE);
    if (!stage) return 0;
    while (done < total) {
        size_t want = (size_t)(total - done < STAGE ? total - done : STAGE);
        size_t got = fread(stage, 1, want, f);
        memcpy((char *)p + done, stage, got);
        done += got;
        if (got < want) break;
    }
    free(stage);
    return done / size;
}
static uint64_t GUEST_ABI g_fwrite(const void *p, uint64_t size, uint64_t n, FILE *f) {
    if (!size || !n) return 0;
    uint64_t total = size * n, done = 0;
    char *stage = malloc(STAGE);
    if (!stage) return 0;
    while (done < total) {
        size_t want = (size_t)(total - done < STAGE ? total - done : STAGE);
        memcpy(stage, (const char *)p + done, want);
        size_t put = fwrite(stage, 1, want, f);
        done += put;
        if (put < want) break;
    }
    free(stage);
    return done / size;
}
static int GUEST_ABI g_fseek(FILE *f, int64_t offset, int whence) { int r = _fseeki64(f, offset, whence); if (r) sync_errno(); return r; }
static int64_t GUEST_ABI g_ftell(FILE *f) { return _ftelli64(f); }
static int GUEST_ABI g_feof(FILE *f) { return feof(f); }
static int GUEST_ABI g_ferror(FILE *f) { return ferror(f); }
static int GUEST_ABI g_fflush(FILE *f) { return fflush(f); }
static int GUEST_ABI g_fileno(FILE *f) { return _fileno(f); }
static int GUEST_ABI g_fputc(int c, FILE *f) { return fputc(c, f); }
static int GUEST_ABI g_fputs(const char *s, FILE *f) { return fputs(s, f); }
static int GUEST_ABI g_puts(const char *s) { return puts(s); }
static int GUEST_ABI g_putchar(int c) { return putchar(c); }
static int GUEST_ABI g_vfprintf(FILE *f, const char *format, SysvVaList *va) { SysvVaList copy = *va; return libc_vfprintf(f, format, &copy); }
static int GUEST_ABI g_vsnprintf(char *d, uint64_t n, const char *format, SysvVaList *va) { SysvVaList copy = *va; return sysv_vformat(d, (size_t)n, format, &copy); }

// ---- stdlib --------------------------------------------------------------------------------------
static void *GUEST_ABI g_malloc(uint64_t n) { return malloc((size_t)n); }
static void *GUEST_ABI g_calloc(uint64_t n, uint64_t size) { return calloc((size_t)n, (size_t)size); }
static void GUEST_ABI g_free(void *p) { free(p); }
typedef int (GUEST_ABI *Compare)(const void *, const void *);
static int __cdecl compare_bridge(void *context, const void *a, const void *b) { return ((Compare)context)(a, b); }
static void GUEST_ABI g_qsort(void *base, uint64_t n, uint64_t size, Compare compare) {
    qsort_s(base, (size_t)n, (size_t)size, compare_bridge, (void *)compare);
}
static void *GUEST_ABI g_bsearch(const void *key, const void *base, uint64_t n, uint64_t size, Compare compare) {
    const char *low = base;
    while (n) {
        const char *middle = low + (n / 2) * size;
        int c = compare(key, middle);
        if (!c) return (void *)middle;
        if (c > 0) { low = middle + size; n -= n / 2 + 1; } else n /= 2;
    }
    return NULL;
}
// 64-bit LCG, upper bits out. RAND_MAX is assumed to be 0x7fffffff (BSD); unverified for the PS4 SDK.
static uint64_t rand_state = 1;
static int GUEST_ABI g_rand(void) {
    uint64_t s = rand_state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    rand_state = s;
    return (int)(s >> 33);
}
static void GUEST_ABI g_srand(unsigned seed) { rand_state = seed; }

typedef void (GUEST_ABI *ExitHandler)(void *);
static struct { ExitHandler f; void *arg; } exit_handlers[256];
static LONG exit_count;
static SRWLOCK exit_lock = SRWLOCK_INIT;
static int GUEST_ABI g_cxa_atexit(ExitHandler f, void *arg, void *dso) {
    (void)dso;
    AcquireSRWLockExclusive(&exit_lock);
    int ok = exit_count < 256;
    if (ok) { exit_handlers[exit_count].f = f; exit_handlers[exit_count].arg = arg; ++exit_count; }
    ReleaseSRWLockExclusive(&exit_lock);
    return ok ? 0 : -1;
}
static int GUEST_ABI g_atexit(void (GUEST_ABI *f)(void)) { return g_cxa_atexit((ExitHandler)f, NULL, NULL); }
static void GUEST_ABI g_exit(int status) {
    for (;;) {
        AcquireSRWLockExclusive(&exit_lock);
        ExitHandler f = NULL; void *arg = NULL;
        if (exit_count) { --exit_count; f = exit_handlers[exit_count].f; arg = exit_handlers[exit_count].arg; }
        ReleaseSRWLockExclusive(&exit_lock);
        if (!f) break;
        f(arg);
    }
    printf("Guest exit(%d)\n", status);
    fflush(NULL);
    ExitProcess((UINT)status);
}
static void GUEST_ABI g_catch_return_from_main(int status) { g_exit(status); }
static void GUEST_ABI g_init_env(void) {}
// Module unload: the handlers registered for that module would run here; modules never unload.
static void GUEST_ABI g_cxa_finalize(void *dso) { (void)dso; }

// ---- time ----------------------------------------------------------------------------------------
// BSD struct tm: the nine ints, then long tm_gmtoff and char *tm_zone.
typedef struct { int32_t sec, min, hour, mday, mon, year, wday, yday, isdst; int64_t gmtoff; const char *zone; } GuestTm;
_Static_assert(sizeof(GuestTm) == 56, "PS4 struct tm layout");
static int64_t GUEST_ABI g_time(int64_t *out) { int64_t t = _time64(NULL); if (out) *out = t; return t; }
static GuestTm to_guest(const struct tm *h, int64_t gmtoff, const char *zone) {
    GuestTm g = {h->tm_sec, h->tm_min, h->tm_hour, h->tm_mday, h->tm_mon, h->tm_year, h->tm_wday, h->tm_yday, h->tm_isdst, gmtoff, zone};
    return g;
}
static struct tm to_host(const GuestTm *t) {
    struct tm h = {0};
    h.tm_sec = t->sec; h.tm_min = t->min; h.tm_hour = t->hour; h.tm_mday = t->mday; h.tm_mon = t->mon;
    h.tm_year = t->year; h.tm_wday = t->wday; h.tm_yday = t->yday; h.tm_isdst = t->isdst;
    return h;
}
// C11 Annex K order (time, result), as in the PS4 libc, not the Microsoft one.
static GuestTm *GUEST_ABI g_gmtime_s(const int64_t *t, GuestTm *out) {
    struct tm h;
    if (!t || !out || _gmtime64_s(&h, t)) { *runtime_errno() = EINVAL; return NULL; }
    *out = to_guest(&h, 0, "UTC");
    return out;
}
static GuestTm *GUEST_ABI g_gmtime(const int64_t *t) {
    static _Thread_local GuestTm result;
    return g_gmtime_s(t, &result);
}
static GuestTm *GUEST_ABI g_localtime(const int64_t *t) {
    static _Thread_local GuestTm result;
    struct tm h;
    if (!t || _localtime64_s(&h, t)) { *runtime_errno() = EINVAL; return NULL; }
    long offset = 0;
    _get_timezone(&offset); // seconds west of UTC, without daylight saving
    result = to_guest(&h, -(int64_t)offset + (h.tm_isdst > 0 ? 3600 : 0), "");
    return &result;
}
static int64_t GUEST_ABI g_mktime(GuestTm *t) {
    struct tm h = to_host(t);
    int64_t r = _mktime64(&h);
    if (r != -1) *t = to_guest(&h, t->gmtoff, t->zone);
    return r;
}
// The UCRT aborts on conversions it does not know, so those are printed literally.
static uint64_t GUEST_ABI g_strftime(char *d, uint64_t n, const char *format, const GuestTm *t) {
    char host[512]; size_t k = 0;
    for (const char *f = format; *f && k + 4 < sizeof(host); ++f) {
        host[k++] = *f;
        if (*f != '%') continue;
        if (f[1] == 'E' || f[1] == 'O') ++f;
        if (f[1] && strchr("aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%", f[1])) host[k++] = *++f;
        else host[k++] = '%';
    }
    host[k] = 0;
    struct tm h = to_host(t);
    return strftime(d, (size_t)n, host, &h);
}

// ---- C++ ABI -------------------------------------------------------------------------------------
// Itanium guard: byte 0 set once initialized; byte 1 marks an initialization in progress.
static SRWLOCK guard_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE guard_done = CONDITION_VARIABLE_INIT;
static int GUEST_ABI g_guard_acquire(volatile uint8_t *g) {
    if (__atomic_load_n(g, __ATOMIC_ACQUIRE)) return 0;
    AcquireSRWLockExclusive(&guard_lock);
    while (g[1]) SleepConditionVariableSRW(&guard_done, &guard_lock, INFINITE, 0);
    int run = !g[0];
    if (run) g[1] = 1;
    ReleaseSRWLockExclusive(&guard_lock);
    return run;
}
static void GUEST_ABI g_guard_release(volatile uint8_t *g) {
    AcquireSRWLockExclusive(&guard_lock);
    __atomic_store_n(g, 1, __ATOMIC_RELEASE);
    g[1] = 0;
    ReleaseSRWLockExclusive(&guard_lock);
    WakeAllConditionVariable(&guard_done);
}
static void GUEST_ABI g_pure_virtual(void) { stop("pure virtual function called"); }

// ---- resolution ----------------------------------------------------------------------------------
static uint32_t need_sce_libc = 1; // Need_sceLibc and Need_sceLibcInternal: link markers
static const struct { const char *api; const void *fn; } table[] = {
#define E(name, fn) {name, (const void *)fn}
    E("strchr", g_strchr), E("strrchr", g_strrchr), E("strstr", g_strstr), E("strcpy", g_strcpy),
    E("strncpy", g_strncpy), E("strncat", g_strncat), E("strspn", g_strspn), E("strcspn", g_strcspn),
    E("strcasecmp", g_strcasecmp), E("strncasecmp", g_strncasecmp), E("memchr", g_memchr), E("wcslen", g_wcslen),
    E("strtof", g_strtof), E("strtol", g_strtoll), E("strtoll", g_strtoll), E("strtoull", g_strtoull),
    E("_Stoul", g_strtoull), E("memcpy_s", g_memcpy_s), E("strncpy_s", g_strncpy_s), E("wcsrtombs", g_wcsrtombs),
    E("_Getpctype", g_getpctype), E("_Getptolower", g_getptolower), E("_Getptoupper", g_getptoupper),
    E("cos", g_cos), E("sin", g_sin), E("tan", g_tan), E("exp", g_exp), E("exp2", g_exp2), E("log", g_log),
    E("log10", g_log10), E("atan2", g_atan2), E("hypot", g_hypot), E("pow", g_pow),
    E("acosf", g_acosf), E("asinf", g_asinf), E("asinhf", g_asinhf), E("atanf", g_atanf), E("cbrtf", g_cbrtf),
    E("cosf", g_cosf), E("erff", g_erff), E("exp2f", g_exp2f), E("expf", g_expf), E("log10f", g_log10f),
    E("log2f", g_log2f), E("logf", g_logf), E("roundf", g_roundf), E("sinf", g_sinf), E("tanf", g_tanf),
    E("tanhf", g_tanhf), E("atan2f", g_atan2f), E("fmodf", g_fmodf), E("nextafterf", g_nextafterf), E("powf", g_powf),
    E("frexpf", g_frexpf), E("ldexpf", g_ldexpf), E("modff", g_modff), E("sincos", g_sincos), E("sincosf", g_sincosf),
    E("__isfinite", g_isfinite), E("__isfinitef", g_isfinitef), E("_FCosh", g_fcosh), E("_FSinh", g_fsinh),
    E("__powidf2", g_powidf2), E("__powisf2", g_powisf2), E("__udivti3", g_udivti3),
    E("fopen", g_fopen), E("fclose", g_fclose), E("fread", g_fread), E("fwrite", g_fwrite), E("fseek", g_fseek),
    E("ftell", g_ftell), E("feof", g_feof), E("ferror", g_ferror), E("fflush", g_fflush), E("fileno", g_fileno),
    E("fputc", g_fputc), E("fputs", g_fputs), E("puts", g_puts), E("putchar", g_putchar),
    E("printf", libc_guest_printf), E("fprintf", libc_guest_fprintf), E("sprintf", libc_guest_sprintf),
    E("snprintf", libc_guest_snprintf), E("sscanf", libc_guest_sscanf), E("vfprintf", g_vfprintf),
    E("vsnprintf", g_vsnprintf),
    E("malloc", g_malloc), E("calloc", g_calloc), E("free", g_free), E("qsort", g_qsort), E("bsearch", g_bsearch),
    E("rand", g_rand), E("srand", g_srand), E("atexit", g_atexit), E("__cxa_atexit", g_cxa_atexit), E("exit", g_exit),
    E("catchReturnFromMain", g_catch_return_from_main), E("_init_env", g_init_env),
    E("time", g_time), E("gmtime", g_gmtime), E("gmtime_s", g_gmtime_s), E("localtime", g_localtime),
    E("mktime", g_mktime), E("strftime", g_strftime), E("__cxa_finalize", g_cxa_finalize),
    E("__cxa_guard_acquire", g_guard_acquire), E("__cxa_guard_release", g_guard_release),
    E("__cxa_pure_virtual", g_pure_virtual),
    E("sceLibcMspaceCreate", mspace_create), E("sceLibcMspaceDestroy", mspace_destroy),
    E("sceLibcMspaceFree", mspace_free), E("sceLibcMspaceMemalign", mspace_memalign),
    E("sceLibcMspaceReallocalign", mspace_reallocalign), E("sceLibcMspaceMallocUsableSize", mspace_usable_size),
    E("sceLibcMspaceMallocStats", mspace_stats), E("sceLibcMspaceMallocStatsFast", mspace_stats),
#undef E
};
static int is_libc(const char *identity) {
    return strstr(identity, "#libc#1#libc#257") || strstr(identity, "#libSceLibcInternal#1#libSceLibcInternal#257");
}
// libSceLibcInternal (what Fios2 imports) always comes from here. The libc identity only does
// when no libc.prx is linked; otherwise the eboot keeps the game's own libc.
uintptr_t libc_resolve(const char *identity) {
    if (!is_libc(identity)) return 0;
    if (strstr(identity, "#libc#1#libc#257") && runtime_bundled_module_count()) return 0;
    const char *api = runtime_api_name(identity);
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (!strcmp(api, table[i].api)) return (uintptr_t)table[i].fn;
    return 0;
}
uintptr_t libc_resolve_data(const char *identity) {
    if (strstr(identity, "#libc#1#libc#257") && runtime_bundled_module_count()) return 0;
    const char *api = runtime_api_name(identity);
    return is_libc(identity) && (!strcmp(api, "Need_sceLibc") || !strcmp(api, "Need_sceLibcInternal")) ? (uintptr_t)&need_sce_libc : 0;
}
