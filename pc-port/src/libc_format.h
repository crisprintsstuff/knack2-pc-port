// SPDX-License-Identifier: GPL-2.0-or-later
// printf/scanf over guest (SysV) argument lists. Free of windows.h so tests also build on Linux.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifndef GUEST_ABI
#define GUEST_ABI __attribute__((sysv_abi))
#endif
#ifdef __cplusplus
extern "C" {
#endif
// The SysV x86-64 va_list record: 6 integer registers (48 bytes) then 8 XMM registers (16 each).
typedef struct { uint32_t gp_offset, fp_offset; uint8_t *overflow, *reg_save; } SysvVaList;
_Static_assert(sizeof(SysvVaList) == 24, "SysV va_list layout");
uint64_t sysv_arg_gp(SysvVaList *va);
double sysv_arg_fp(SysvVaList *va);
// vsnprintf semantics: writes at most cap bytes (terminated when cap > 0), returns the full length.
int sysv_vformat(char *out, size_t cap, const char *format, SysvVaList *va);
// vsscanf semantics, for up to 32 assignments.
int sysv_vscan(const char *input, const char *format, SysvVaList *va);
// Bodies of the variadic guest entry points in libc_varargs.S. Each receives every argument,
// fixed ones included, through va.
int GUEST_ABI libc_printf_va(SysvVaList *va);
int GUEST_ABI libc_fprintf_va(SysvVaList *va);
int GUEST_ABI libc_sprintf_va(SysvVaList *va);
int GUEST_ABI libc_snprintf_va(SysvVaList *va);
int GUEST_ABI libc_sscanf_va(SysvVaList *va);
int libc_vfprintf(FILE *file, const char *format, SysvVaList *va);
// The variadic entry points themselves (SysV, libc_varargs.S).
int libc_guest_printf(void);
int libc_guest_fprintf(void);
int libc_guest_sprintf(void);
int libc_guest_snprintf(void);
int libc_guest_sscanf(void);
#ifdef __cplusplus
}
#endif
