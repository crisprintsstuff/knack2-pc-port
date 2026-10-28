// SPDX-License-Identifier: GPL-2.0-or-later
// Entered by the eboot's five-byte SysV CALL at +0x3F070C. Preserve its full
// integer, flags and AVX argument state while making a Windows ABI callback,
// then tail-jump to the original callee with the original stack arguments.
.intel_syntax noprefix
.text
.globl iss_feedback_identity_stub
.def iss_feedback_identity_stub
  .scl 2
  .type 32
.endef
iss_feedback_identity_stub:
  pushfq
  push rax
  push rcx
  push rdx
  push rbx
  push rbp
  push rsi
  push rdi
  push r8
  push r9
  push r10
  push r11
  push r12
  push r13
  push r14
  push r15
  // RSP mod 16 was 8 on entry and is still 8 after sixteen pushes.
  // 0x20 bytes are reserved for Win64 shadow space. 16 YMM registers occupy
  // the next 0x200 bytes; 8 bytes pad/alignment bring RSP mod 16 to zero.
  sub rsp, 0x228
  vmovdqu ymmword ptr [rsp+0x020], ymm0
  vmovdqu ymmword ptr [rsp+0x040], ymm1
  vmovdqu ymmword ptr [rsp+0x060], ymm2
  vmovdqu ymmword ptr [rsp+0x080], ymm3
  vmovdqu ymmword ptr [rsp+0x0a0], ymm4
  vmovdqu ymmword ptr [rsp+0x0c0], ymm5
  vmovdqu ymmword ptr [rsp+0x0e0], ymm6
  vmovdqu ymmword ptr [rsp+0x100], ymm7
  vmovdqu ymmword ptr [rsp+0x120], ymm8
  vmovdqu ymmword ptr [rsp+0x140], ymm9
  vmovdqu ymmword ptr [rsp+0x160], ymm10
  vmovdqu ymmword ptr [rsp+0x180], ymm11
  vmovdqu ymmword ptr [rsp+0x1a0], ymm12
  vmovdqu ymmword ptr [rsp+0x1c0], ymm13
  vmovdqu ymmword ptr [rsp+0x1e0], ymm14
  vmovdqu ymmword ptr [rsp+0x200], ymm15
  stmxcsr dword ptr [rsp+0x220]
  fnstcw word ptr [rsp+0x224]

  // Windows x64: RCX/RDX/R8. The original SysV RDX is the actual record.
  mov rcx, r12
  mov r8, rdx
  mov rdx, r15
  call iss_feedback_identity_bridge

  ldmxcsr dword ptr [rsp+0x220]
  fldcw word ptr [rsp+0x224]
  vmovdqu ymm0, ymmword ptr [rsp+0x020]
  vmovdqu ymm1, ymmword ptr [rsp+0x040]
  vmovdqu ymm2, ymmword ptr [rsp+0x060]
  vmovdqu ymm3, ymmword ptr [rsp+0x080]
  vmovdqu ymm4, ymmword ptr [rsp+0x0a0]
  vmovdqu ymm5, ymmword ptr [rsp+0x0c0]
  vmovdqu ymm6, ymmword ptr [rsp+0x0e0]
  vmovdqu ymm7, ymmword ptr [rsp+0x100]
  vmovdqu ymm8, ymmword ptr [rsp+0x120]
  vmovdqu ymm9, ymmword ptr [rsp+0x140]
  vmovdqu ymm10, ymmword ptr [rsp+0x160]
  vmovdqu ymm11, ymmword ptr [rsp+0x180]
  vmovdqu ymm12, ymmword ptr [rsp+0x1a0]
  vmovdqu ymm13, ymmword ptr [rsp+0x1c0]
  vmovdqu ymm14, ymmword ptr [rsp+0x1e0]
  vmovdqu ymm15, ymmword ptr [rsp+0x200]
  add rsp, 0x228
  pop r15
  pop r14
  pop r13
  pop r12
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rbp
  pop rbx
  pop rdx
  pop rcx
  pop rax
  popfq
  jmp qword ptr [rip+iss_feedback_identity_target]
