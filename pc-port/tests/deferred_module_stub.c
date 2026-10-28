// SPDX-License-Identifier: GPL-2.0-or-later
// The second-stage entry point is defined in probe.c, which every test binary replaces with the
// test's own main. kernel.c's sceKernelLoadStartModule calls it, so without this the test links
// fail on an undefined symbol. The tests do not exercise a deferred load, so this reports that no
// deferred module is staged, which is the truth here. The probe links its real definition.
#include "runtime.h"
int32_t runtime_start_deferred_module(uint64_t argc, const void *argv, int32_t *result) {
    (void)argc; (void)argv;
    if (result) *result = 0;
    return (int32_t)UINT32_C(0x80020002); // SCE_KERNEL_ERROR_ENOENT
}
