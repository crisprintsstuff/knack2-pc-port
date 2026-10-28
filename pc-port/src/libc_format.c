// SPDX-License-Identifier: GPL-2.0-or-later
// The host CRT cannot read a SysV va_list, so each conversion is fetched here by its PS4 type
// (long is 64-bit, long double is x87 extended) and formatted alone by the host with an
// explicit 64-bit length. %n is handled here; the Windows CRT disables it.
#include "libc_format.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

uint64_t sysv_arg_gp(SysvVaList *va) {
    uint64_t v;
    if (va->gp_offset < 48) { memcpy(&v, va->reg_save + va->gp_offset, 8); va->gp_offset += 8; }
    else { memcpy(&v, va->overflow, 8); va->overflow += 8; }
    return v;
}
double sysv_arg_fp(SysvVaList *va) {
    double v;
    if (va->fp_offset < 176) { memcpy(&v, va->reg_save + va->fp_offset, 8); va->fp_offset += 16; }
    else { memcpy(&v, va->overflow, 8); va->overflow += 8; }
    return v;
}
// long double is passed in memory, 16-byte aligned, as an 80-bit x87 value. Rounded to double.
static double sysv_arg_long_double(SysvVaList *va) {
    uint8_t *p = (uint8_t *)(((uintptr_t)va->overflow + 15) & ~(uintptr_t)15);
    va->overflow = p + 16;
    uint64_t mantissa; uint16_t sign_exponent;
    memcpy(&mantissa, p, 8); memcpy(&sign_exponent, p + 8, 2);
    int exponent = sign_exponent & 0x7fff;
    double v;
    if (exponent == 0x7fff) v = (mantissa << 1) ? NAN : INFINITY;
    else v = ldexp((double)mantissa, (exponent ? exponent : 1) - 16383 - 63);
    return sign_exponent >> 15 ? -v : v;
}

typedef struct { char *out; size_t cap, len; } Sink;
static void put(Sink *s, const char *text, size_t n) {
    if (s->len < s->cap) {
        size_t room = s->cap - s->len, k = n < room ? n : room;
        memcpy(s->out + s->len, text, k);
    }
    s->len += n;
}
static void emit(Sink *s, const char *spec, ...) {
    va_list a, b;
    va_start(a, spec); va_copy(b, a);
    char small[256];
    int n = vsnprintf(small, sizeof(small), spec, a);
    if (n >= 0 && (size_t)n < sizeof(small)) put(s, small, (size_t)n);
    else if (n >= 0) {
        char *big = malloc((size_t)n + 1);
        if (big) { vsnprintf(big, (size_t)n + 1, spec, b); put(s, big, (size_t)n); free(big); }
    }
    va_end(b); va_end(a);
}

enum { L_NONE, L_HH, L_H, L_L, L_LL, L_LD };

int sysv_vformat(char *out, size_t cap, const char *f, SysvVaList *va) {
    Sink s = {out, cap, 0};
    while (*f) {
        if (*f != '%') {
            const char *next = strchr(f, '%');
            size_t n = next ? (size_t)(next - f) : strlen(f);
            put(&s, f, n); f += n; continue;
        }
        const char *start = f++;
        if (*f == '%') { put(&s, "%", 1); ++f; continue; }
        char spec[64]; size_t k = 0;
        spec[k++] = '%';
        while (*f && strchr("-+ #0'", *f)) { if (*f != '\'' && k < 8) spec[k++] = *f; ++f; }
        if (*f == '*') { k += (size_t)snprintf(spec + k, 16, "%d", (int)sysv_arg_gp(va)); ++f; }
        else while (*f >= '0' && *f <= '9') { if (k < 24) spec[k++] = *f; ++f; }
        if (*f == '.') {
            ++f;
            if (*f == '*') { int p = (int)sysv_arg_gp(va); if (p >= 0) k += (size_t)snprintf(spec + k, 16, ".%d", p); ++f; }
            else { spec[k++] = '.'; while (*f >= '0' && *f <= '9') { if (k < 40) spec[k++] = *f; ++f; } }
        }
        int length = L_NONE;
        if (*f == 'h') { ++f; length = L_H; if (*f == 'h') { ++f; length = L_HH; } }
        else if (*f == 'l') { ++f; length = L_L; if (*f == 'l') { ++f; length = L_LL; } }
        else if (*f == 'j' || *f == 'z' || *f == 't' || *f == 'q') { ++f; length = L_LL; }
        else if (*f == 'L') { ++f; length = L_LD; }
        char conversion = *f;
        if (!conversion) { put(&s, start, strlen(start)); break; }
        ++f;
        switch (conversion) {
        case 'd': case 'i': {
            int64_t v = (int64_t)sysv_arg_gp(va);
            if (length == L_HH) v = (signed char)v; else if (length == L_H) v = (short)v;
            else if (length == L_NONE) v = (int32_t)v;
            memcpy(spec + k, "lld", 4); emit(&s, spec, (long long)v); break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = sysv_arg_gp(va);
            if (length == L_HH) v = (unsigned char)v; else if (length == L_H) v = (unsigned short)v;
            else if (length == L_NONE) v = (uint32_t)v;
            spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = conversion; spec[k + 3] = 0;
            emit(&s, spec, (unsigned long long)v); break;
        }
        case 'c':
            if (length == L_L) { memcpy(spec + k, "lc", 3); emit(&s, spec, (wint_t)(uint16_t)sysv_arg_gp(va)); }
            else { memcpy(spec + k, "c", 2); emit(&s, spec, (int)sysv_arg_gp(va)); }
            break;
        case 's': {
            const void *p = (const void *)(uintptr_t)sysv_arg_gp(va);
            // PS4 wchar_t is 16-bit, as on Windows.
            if (length == L_L) { memcpy(spec + k, "ls", 3); emit(&s, spec, (const wchar_t *)p); }
            else { memcpy(spec + k, "s", 2); emit(&s, spec, p ? (const char *)p : "(null)"); }
            break;
        }
        case 'p': {
            char text[24];
            snprintf(text, sizeof(text), "0x%llx", (unsigned long long)sysv_arg_gp(va));
            memcpy(spec + k, "s", 2); emit(&s, spec, text); break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = length == L_LD ? sysv_arg_long_double(va) : sysv_arg_fp(va);
            spec[k] = conversion; spec[k + 1] = 0; emit(&s, spec, v); break;
        }
        case 'n': {
            void *p = (void *)(uintptr_t)sysv_arg_gp(va);
            if (!p) break;
            if (length == L_HH) *(signed char *)p = (signed char)s.len;
            else if (length == L_H) *(short *)p = (short)s.len;
            else if (length == L_L || length == L_LL) *(int64_t *)p = (int64_t)s.len;
            else *(int32_t *)p = (int32_t)s.len;
            break;
        }
        default: put(&s, start, (size_t)(f - start)); break;
        }
    }
    if (cap) s.out[s.len < cap ? s.len : cap - 1] = 0;
    return s.len > INT32_MAX ? -1 : (int)s.len;
}

// Rewrites the format for the host (64-bit l/j/z/t become ll) and passes the guest's pointers on.
int sysv_vscan(const char *input, const char *f, SysvVaList *va) {
    char host[1024]; size_t k = 0;
    void *p[32] = {0}; int count = 0;
#define OUT(c) do { if (k + 1 >= sizeof(host)) return -1; host[k++] = (c); } while (0)
    while (*f) {
        if (*f != '%') { OUT(*f++); continue; }
        OUT(*f++);
        if (*f == '%') { OUT(*f++); continue; }
        int assign = *f != '*';
        if (!assign) OUT(*f++);
        while (*f >= '0' && *f <= '9') OUT(*f++);
        int length = L_NONE;
        if (*f == 'h') { ++f; length = L_H; if (*f == 'h') { ++f; length = L_HH; } }
        else if (*f == 'l') { ++f; length = L_L; if (*f == 'l') { ++f; length = L_LL; } }
        else if (*f == 'j' || *f == 'z' || *f == 't' || *f == 'q') { ++f; length = L_LL; }
        else if (*f == 'L') return -1; // x87 long double has no host equivalent
        char c = *f;
        if (!c) return -1;
        int integer = strchr("diouxXn", c) != NULL;
        if (length == L_HH) { OUT('h'); OUT('h'); }
        else if (length == L_H) OUT('h');
        else if (length == L_LL || (length == L_L && integer)) { OUT('l'); OUT('l'); }
        else if (length == L_L) OUT('l');
        OUT(*f++);
        if (c == '[') {
            if (*f == '^') OUT(*f++);
            if (*f == ']') OUT(*f++);
            while (*f && *f != ']') OUT(*f++);
            if (!*f) return -1;
            OUT(*f++);
        }
        if (assign) { if (count == 32) return -1; p[count++] = (void *)(uintptr_t)sysv_arg_gp(va); }
    }
#undef OUT
    host[k] = 0;
    return sscanf(input, host, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10],
                  p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[20], p[21],
                  p[22], p[23], p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
}

int libc_vfprintf(FILE *file, const char *format, SysvVaList *va) {
    SysvVaList measure = *va;
    char small[1024];
    int n = sysv_vformat(small, sizeof(small), format, &measure);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(small)) return fwrite(small, 1, (size_t)n, file) == (size_t)n ? n : -1;
    char *big = malloc((size_t)n + 1);
    if (!big) return -1;
    sysv_vformat(big, (size_t)n + 1, format, va);
    int written = fwrite(big, 1, (size_t)n, file) == (size_t)n ? n : -1;
    free(big);
    return written;
}
int GUEST_ABI libc_printf_va(SysvVaList *va) {
    const char *format = (const char *)(uintptr_t)sysv_arg_gp(va);
    return libc_vfprintf(stdout, format, va);
}
int GUEST_ABI libc_fprintf_va(SysvVaList *va) {
    FILE *file = (FILE *)(uintptr_t)sysv_arg_gp(va);
    const char *format = (const char *)(uintptr_t)sysv_arg_gp(va);
    return libc_vfprintf(file, format, va);
}
int GUEST_ABI libc_sprintf_va(SysvVaList *va) {
    char *out = (char *)(uintptr_t)sysv_arg_gp(va);
    const char *format = (const char *)(uintptr_t)sysv_arg_gp(va);
    return sysv_vformat(out, SIZE_MAX / 2, format, va);
}
int GUEST_ABI libc_snprintf_va(SysvVaList *va) {
    char *out = (char *)(uintptr_t)sysv_arg_gp(va);
    size_t cap = (size_t)sysv_arg_gp(va);
    const char *format = (const char *)(uintptr_t)sysv_arg_gp(va);
    return sysv_vformat(out, cap, format, va);
}
int GUEST_ABI libc_sscanf_va(SysvVaList *va) {
    const char *input = (const char *)(uintptr_t)sysv_arg_gp(va);
    const char *format = (const char *)(uintptr_t)sysv_arg_gp(va);
    return sysv_vscan(input, format, va);
}
