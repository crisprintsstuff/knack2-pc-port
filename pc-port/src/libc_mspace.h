// SPDX-License-Identifier: GPL-2.0-or-later
// sceLibcMspace*: allocators over a memory range the game provides, so the blocks stay inside
// guest mappings the renderer can see. Free of windows.h so tests also build on Linux.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef GUEST_ABI
#define GUEST_ABI __attribute__((sysv_abi))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Mspace Mspace;
// SceLibcMallocManagedSize.
typedef struct {
    uint16_t size, version; uint32_t reserved;
    uint64_t max_system, current_system, max_inuse, current_inuse;
} MspaceStats;
_Static_assert(sizeof(MspaceStats) == 40, "SceLibcMallocManagedSize layout");
Mspace *GUEST_ABI mspace_create(const char *name, void *base, uint64_t capacity, uint32_t flags);
int32_t GUEST_ABI mspace_destroy(Mspace *m);
void *GUEST_ABI mspace_memalign(Mspace *m, uint64_t boundary, uint64_t size);
void *GUEST_ABI mspace_reallocalign(Mspace *m, void *p, uint64_t boundary, uint64_t size);
void GUEST_ABI mspace_free(Mspace *m, void *p);
uint64_t GUEST_ABI mspace_usable_size(void *p);
int32_t GUEST_ABI mspace_stats(Mspace *m, MspaceStats *out);
// Walks every block and checks the boundary tags and free lists; 0 when consistent.
int mspace_check(Mspace *m);
#ifdef __cplusplus
}
#endif
