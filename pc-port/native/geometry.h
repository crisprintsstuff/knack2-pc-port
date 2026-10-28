// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#ifndef ISS_SYSV
#define ISS_SYSV __attribute__((sysv_abi))
#endif

namespace iss::native {
// CPU contract reconstructed from CUSA00004 eboot+0x1A1960. The lower-level binding
// writer remains an adapter, so this port preserves the existing submission order.
struct GeometryBindings {
    const std::uintptr_t* default_resource_base;
    const std::uint8_t* index_marker_enabled;
    void (ISS_SYSV *vertex_buffers)(void*, std::uint32_t, std::uint32_t,
                                    std::uint32_t, const void*);
    void (ISS_SYSV *instance_data)(void*, std::uint32_t, std::uint32_t,
                                  std::uint32_t, const void*);
    void (ISS_SYSV *primitive)(void*, std::uint32_t);
    void (ISS_SYSV *index_type)(void*, std::uint32_t, std::uint32_t);
    void (ISS_SYSV *index_marker)(void*, std::uint32_t);
    void (ISS_SYSV *flush_bindings)(void*);
    void (ISS_SYSV *draw_indexed)(void*, std::uint32_t, std::uint64_t, std::uint32_t);
};

// False means no call or write was emitted: the caller must execute the original routine.
// This guard covers layouts outside the original routine's 32-descriptor scratch capacity.
bool EmitIndexedGeometry(const GeometryBindings&, const void* material, void* context,
                         const void* object);
int InstallIndexedGeometry(unsigned char* image, std::size_t image_size);
// ISS_ENGINE_MARKERS=1 (engine_draw.cpp).
int InstallEngineMarkers(unsigned char* image, std::size_t image_size);
// ISS_RENDER_JOB_CENSUS=1: bounded, observation-only frame job entry/exit census.
int InstallRenderJobCensus(unsigned char* image, std::size_t image_size);
}
