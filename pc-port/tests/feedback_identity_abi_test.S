// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.globl iss_feedback_test_call
.def iss_feedback_test_call
  .scl 2
  .type 32
.endef
// Windows ABI: (owner, shader, record, near_bridge). Recreates the SysV
// register and stack state at eboot+0x3F070C, then calls the installed bridge.
iss_feedback_test_call:
  push rbx
  push rdi
  push rsi
  push r12
  push r15
  sub rsp, 0x20
  mov r12, rcx
  mov r15, rdx
  mov rbx, r9
  mov rdx, r8
  mov rdi, 0x11111111
  mov rsi, 0x22222222
  mov rcx, 0x44444444
  mov r8, 0x55555555
  mov r9, 0x66666666
  mov qword ptr [rsp], 0x77777777
  mov eax, 0x88888888
  mov [rsp+8], rax
  mov eax, 0x99999999
  mov [rsp+16], rax
  vmovdqu ymm0, ymmword ptr [rip+iss_feedback_test_ymm_seed]
  stc
  call rbx
  add rsp, 0x20
  pop r15
  pop r12
  pop rsi
  pop rdi
  pop rbx
  ret

.globl iss_feedback_test_original
.def iss_feedback_test_original
  .scl 2
  .type 32
.endef
iss_feedback_test_original:
  lea rax, [rip+iss_feedback_test_observed]
  mov [rax+0x00], r12
  mov [rax+0x08], r15
  mov [rax+0x10], rdx
  mov [rax+0x18], rdi
  mov [rax+0x20], rsi
  mov [rax+0x28], rcx
  mov [rax+0x30], r8
  mov [rax+0x38], r9
  mov r10, [rsp+8]
  mov [rax+0x40], r10
  mov r10, [rsp+16]
  mov [rax+0x48], r10
  mov r10, [rsp+24]
  mov [rax+0x50], r10
  pushfq
  pop r10
  mov [rax+0x58], r10
  vmovdqu ymmword ptr [rip+iss_feedback_test_seen_ymm], ymm0
  mov eax, 0x4f
  ret
