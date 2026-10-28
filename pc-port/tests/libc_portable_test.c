// SPDX-License-Identifier: GPL-2.0-or-later
// Tests the guest printf/scanf entry points and the mspace allocator. The entry points take
// SysV arguments, which is the native ABI on x86-64 Linux, so there they are called directly:
//   clang -O2 -Wall -Wextra -Werror -Isrc tests/libc_portable_test.c src/libc_format.c
//         src/libc_mspace.c src/libc_varargs.S -lm -o libc-test
// On Windows the build calls them through SysV function pointers instead.
#include "../src/libc_format.h"
#include "../src/libc_mspace.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (GUEST_ABI *SnprintfFn)(char *, uint64_t, const char *, ...);
typedef int (GUEST_ABI *SprintfFn)(char *, const char *, ...);
typedef int (GUEST_ABI *SscanfFn)(const char *, const char *, ...);
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void format(const char *expected, int expected_length, const char *actual, int length) {
    if (strcmp(expected, actual) || length != expected_length) {
        printf("FAIL format: expected \"%s\" (%d), got \"%s\" (%d)\n", expected, expected_length, actual, length);
        ++failures;
    }
}

static void test_format(void) {
    SnprintfFn sn = (SnprintfFn)(void *)libc_guest_snprintf;
    SprintfFn sp = (SprintfFn)(void *)libc_guest_sprintf;
    char b[256];
    int n;
    n = sn(b, sizeof(b), "%d %i %u %x %X %o", -5, 42, 4000000000u, 0xbeef, 0xBEEF, 8); format("-5 42 4000000000 beef BEEF 10", 29, b, n);
    // long is 64-bit on the PS4 (not on Windows), so guest longs are passed as int64_t here.
    n = sn(b, sizeof(b), "%ld %lu %lld %zu %jd", INT64_C(-9000000000), UINT64_C(18000000000), -1LL, (uint64_t)7, (int64_t)-2); format("-9000000000 18000000000 -1 7 -2", 31, b, n);
    n = sn(b, sizeof(b), "%hhd %hd %hhu", 300, 70000, 511); format("44 4464 255", 11, b, n);
    n = sn(b, sizeof(b), "[%5d|%-5d|%05d|%+d|% d]", 42, 42, 42, 42, 42); format("[   42|42   |00042|+42| 42]", 27, b, n);
    n = sn(b, sizeof(b), "[%*d|%-*d|%.*f|%.*s]", 6, 1, 4, 2, 2, 3.14159, 3, "abcdef"); format("[     1|2   |3.14|abc]", 22, b, n);
    n = sn(b, sizeof(b), "%.*f", -1, 1.5); format("1.500000", 8, b, n);
    n = sn(b, sizeof(b), "%f %.2e %g %G", 1.5, 12345.678, 0.0001, 1e20); format("1.500000 1.23e+04 0.0001 1E+20", 30, b, n);
    n = sn(b, sizeof(b), "%s|%c|%%|%s", "hi", 'x', (char *)NULL); format("hi|x|%|(null)", 13, b, n);
    n = sn(b, sizeof(b), "%p %p", (void *)0x1234, (void *)0); format("0x1234 0x0", 10, b, n);
    // More doubles than the 8 XMM registers, and more integers than the 6 GP registers.
    n = sn(b, sizeof(b), "%g %g %g %g %g %g %g %g %g %g %d %d %d %d", 1., 2., 3., 4., 5., 6., 7., 8., 9., 10., 11, 12, 13, 14);
    format("1 2 3 4 5 6 7 8 9 10 11 12 13 14", 32, b, n);
    int count = 0; long long big = 0;
    n = sn(b, sizeof(b), "abc%n defg%lln", &count, &big); format("abc defg", 8, b, n);
    CHECK(count == 3 && big == 8);
    n = sn(b, 5, "%s", "truncated"); format("trun", 9, b, n);
    n = sn(NULL, 0, "%d", 12345); CHECK(n == 5);
    n = sp(b, "%s-%d", "x", 9); format("x-9", 3, b, n);
    n = sn(b, sizeof(b), "%q", 1); format("%q", 2, b, n);
}

// A PS4 caller passes long double as an 80-bit x87 value in memory, 16-byte aligned. long double is
// only a 64-bit double on Windows, so that argument list is built by hand.
static void test_long_double(void) {
    _Alignas(16) uint8_t stack[32] = {0}, registers[176] = {0};
    uint64_t mantissa = UINT64_C(0x9000000000000000), three = 3; // 2.25 = 1.125 * 2^1
    uint16_t sign_exponent = 0x4000;
    memcpy(stack, &mantissa, 8); memcpy(stack + 8, &sign_exponent, 2); memcpy(stack + 16, &three, 8);
    SysvVaList va = {48, 176, stack, registers}; // registers exhausted: everything comes from memory
    char b[64];
    int n = sysv_vformat(b, sizeof(b), "%Lf %d", &va);
    format("2.250000 3", 10, b, n);
    sign_exponent = 0xc000; memcpy(stack + 8, &sign_exponent, 2);
    va = (SysvVaList){48, 176, stack, registers};
    n = sysv_vformat(b, sizeof(b), "%.1Lf", &va);
    format("-2.2", 4, b, n);
}

static void test_scan(void) {
    SscanfFn ss = (SscanfFn)(void *)libc_guest_sscanf;
    int i = 0; int64_t l = 0; unsigned long long u = 0; float f = 0; double d = 0; char s[16] = {0}, set[16] = {0};
    short h = 0; signed char c = 0; int consumed = 0;
    int n = ss("12 -9000000000 18000000000 2.5 3.25 word abc 7 9 rest", "%d %ld %llu %f %lf %15s %15[a-z] %hd %hhd%n",
               &i, &l, &u, &f, &d, s, set, &h, &c, &consumed);
    CHECK(n == 9); CHECK(i == 12); CHECK(l == INT64_C(-9000000000)); CHECK(u == 18000000000ULL);
    CHECK(f == 2.5f); CHECK(d == 3.25); CHECK(!strcmp(s, "word")); CHECK(!strcmp(set, "abc"));
    CHECK(h == 7); CHECK(c == 9); CHECK(consumed == 48);
    int a = 0, z = 0;
    CHECK(ss("1 2", "%*d %d", &a) == 1 && a == 2);
    CHECK(ss("x", "%d", &z) == 0);
}

static void test_mspace(void) {
    enum { SIZE = 1 << 22, SLOTS = 512 };
    unsigned char *region = malloc(SIZE + 8);
    Mspace *m = mspace_create("test", region + 8, SIZE, 0);
    CHECK(m != NULL);
    MspaceStats st = {0};
    CHECK(mspace_stats(m, &st) == 0 && st.size == 40 && st.current_inuse == 0);
    void *slot[SLOTS] = {0}; size_t size[SLOTS] = {0};
    srand(1);
    for (int step = 0; step < 200000; ++step) {
        int k = rand() % SLOTS;
        if (slot[k]) {
            for (size_t j = 0; j < size[k]; ++j)
                if (((unsigned char *)slot[k])[j] != (unsigned char)(k + j)) { CHECK(!"payload corrupted"); return; }
            if (rand() & 1) { mspace_free(m, slot[k]); slot[k] = NULL; continue; }
            size_t grow = (size_t)(rand() % 3000);
            size_t align = (size_t)16 << (rand() % 6);
            void *moved = mspace_reallocalign(m, slot[k], align, grow);
            if (!grow) { CHECK(moved == NULL); slot[k] = NULL; continue; }
            if (!moved) continue;
            CHECK(!((uintptr_t)moved & (align - 1)));
            for (size_t j = size[k]; j < grow; ++j) ((unsigned char *)moved)[j] = (unsigned char)(k + j);
            slot[k] = moved; size[k] = grow;
        } else {
            size_t align = (size_t)16 << (rand() % 9), want = (size_t)(rand() % (rand() % 8 ? 512 : 40000));
            void *p = mspace_memalign(m, align, want);
            if (!p) continue;
            CHECK(region + 8 <= (unsigned char *)p && (unsigned char *)p + want <= region + 8 + SIZE);
            CHECK(!((uintptr_t)p & (align - 1)));
            CHECK(mspace_usable_size(p) >= want);
            for (size_t j = 0; j < want; ++j) ((unsigned char *)p)[j] = (unsigned char)(k + j);
            slot[k] = p; size[k] = want;
        }
        if (step % 5000 == 0) { int r = mspace_check(m); if (r) { printf("mspace_check=%d at %d\n", r, step); CHECK(!"consistent"); return; } }
    }
    for (int k = 0; k < SLOTS; ++k) mspace_free(m, slot[k]);
    CHECK(mspace_check(m) == 0);
    CHECK(mspace_stats(m, &st) == 0 && st.current_inuse == 0 && st.max_inuse > 0);
    // Everything coalesced again: the whole range is one free block.
    void *all = mspace_memalign(m, 16, SIZE - 64);
    CHECK(all != NULL);
    CHECK(mspace_memalign(m, 16, 1 << 20) == NULL);
    mspace_free(m, all);
    CHECK(mspace_check(m) == 0);
    CHECK(mspace_destroy(m) == 0);
    free(region);
}

int main(void) {
    test_format();
    test_long_double();
    test_scan();
    test_mspace();
    printf(failures ? "%d FAILED\n" : "libc portable tests passed\n", failures);
    return failures != 0;
}
