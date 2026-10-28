// SPDX-License-Identifier: GPL-2.0-or-later
// Variadic libc entry points called by the guest with the SysV ABI. Each one spills the six
// integer and eight XMM argument registers into a register save area, builds a SysV va_list
// over it (gp_offset 0, so the fixed arguments are read through it too) and passes it to a
// SysV C body in libc_format.c.
.intel_syntax noprefix
.text
#if defined(_WIN32)
#define FUNCTION(name) .globl name; .def name; .scl 2; .type 32; .endef; name:
#else
#define FUNCTION(name) .globl name; .type name, @function; name:
#endif

// RSP mod 16 is 8 on entry; 216 = 13*16+8 aligns it. Layout: [0,24) va_list,
// [32,80) integer registers, [80,208) XMM registers. Stack arguments start at RSP+224.
.macro SYSV_VARIADIC name, body
FUNCTION(\name)
  sub rsp, 216
  mov [rsp+32], rdi
  mov [rsp+40], rsi
  mov [rsp+48], rdx
  mov [rsp+56], rcx
  mov [rsp+64], r8
  mov [rsp+72], r9
  movaps [rsp+80], xmm0
  movaps [rsp+96], xmm1
  movaps [rsp+112], xmm2
  movaps [rsp+128], xmm3
  movaps [rsp+144], xmm4
  movaps [rsp+160], xmm5
  movaps [rsp+176], xmm6
  movaps [rsp+192], xmm7
  mov dword ptr [rsp], 0
  mov dword ptr [rsp+4], 48
  lea rax, [rsp+224]
  mov [rsp+8], rax
  lea rax, [rsp+32]
  mov [rsp+16], rax
  mov rdi, rsp
  call \body
  add rsp, 216
  ret
.endm

SYSV_VARIADIC libc_guest_printf, libc_printf_va
SYSV_VARIADIC libc_guest_fprintf, libc_fprintf_va
SYSV_VARIADIC libc_guest_sprintf, libc_sprintf_va
SYSV_VARIADIC libc_guest_snprintf, libc_snprintf_va
SYSV_VARIADIC libc_guest_sscanf, libc_sscanf_va
