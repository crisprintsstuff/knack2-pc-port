// SPDX-License-Identifier: GPL-2.0-or-later
#include "geometry.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
iss::native::GeometryBindings bindings{};
using Original = void (ISS_SYSV *)(const void*, void*, const void*);
Original original{};
bool statistics{};
std::atomic<std::uint64_t> calls{}, fallbacks{};
void ISS_SYSV Geometry(const void* material, void* context, const void* object) {
    const bool native = iss::native::EmitIndexedGeometry(bindings, material, context, object);
    if (!native) original(material, context, object);
    if (statistics) {
        if (!native) ++fallbacks;
        const auto count = ++calls;
        if (count == 1 || (count & (count - 1)) == 0) {
            std::fprintf(stderr, "NATIVE_GEOMETRY calls=%llu fallbacks=%llu\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(fallbacks.load()));
        }
    }
}
std::uint64_t Hash(const unsigned char* bytes, std::size_t size) {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}
std::array<unsigned char, 14> Jump(const void* target) {
    std::array<unsigned char, 14> result{0xff, 0x25, 0, 0, 0, 0};
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(result.data() + 6, &address, sizeof(address));
    return result;
}
}

int iss::native::InstallIndexedGeometry(unsigned char* image, std::size_t image_size) {
    const char* enabled = std::getenv("ISS_NATIVE_GEOMETRY");
    if (!enabled || enabled[0] != '1') return 0;
    constexpr std::size_t entry = 0x1a1960, length = 549, prologue = 20;
    if (!image || image_size <= 0x1d0cbd5 ||
        Hash(image + entry, length) != 0xea1442f922a0a0ecULL) {
        std::fputs("NATIVE_GEOMETRY rejected: original function identity mismatch\n", stderr);
        return -1;
    }
    // These 20 complete instructions bytes contain no RIP-relative operand. All subsequent
    // original instructions run at their original addresses on the fallback path.
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!trampoline) return -1;
    std::memcpy(trampoline, image + entry, prologue);
    const auto back = Jump(image + entry + prologue);
    std::memcpy(trampoline + prologue, back.data(), back.size());
    DWORD protection;
    if (!VirtualProtect(trampoline, 4096, PAGE_EXECUTE_READ, &protection) ||
        !FlushInstructionCache(GetCurrentProcess(), trampoline, prologue + back.size())) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return -1;
    }
    bindings.default_resource_base = reinterpret_cast<const std::uintptr_t*>(image + 0xd9dec8);
    bindings.index_marker_enabled = image + 0x1d0cbd5;
    bindings.vertex_buffers = reinterpret_cast<decltype(bindings.vertex_buffers)>(image + 0x16aa0);
    bindings.instance_data = reinterpret_cast<decltype(bindings.instance_data)>(image + 0x16b70);
    bindings.primitive = reinterpret_cast<decltype(bindings.primitive)>(image + 0x623740);
    bindings.index_type = reinterpret_cast<decltype(bindings.index_type)>(image + 0x624f80);
    bindings.index_marker = reinterpret_cast<decltype(bindings.index_marker)>(image + 0x628ae0);
    bindings.flush_bindings = reinterpret_cast<decltype(bindings.flush_bindings)>(image + 0x14ad0);
    bindings.draw_indexed = reinterpret_cast<decltype(bindings.draw_indexed)>(image + 0x623c60);
    original = reinterpret_cast<Original>(trampoline);
    const char* stats = std::getenv("ISS_NATIVE_GEOMETRY_STATS");
    statistics = stats && stats[0] == '1';
    std::array<unsigned char, prologue> patch;
    patch.fill(0x90);
    const auto jump = Jump(reinterpret_cast<const void*>(&Geometry));
    std::memcpy(patch.data(), jump.data(), jump.size());
    DWORD old, ignored;
    if (!VirtualProtect(image + entry, patch.size(), PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        original = nullptr;
        return -1;
    }
    std::memcpy(image + entry, patch.data(), patch.size());
    if (!VirtualProtect(image + entry, patch.size(), old, &ignored) ||
        !FlushInstructionCache(GetCurrentProcess(), image + entry, patch.size())) return -1;
    std::fputs("NATIVE_GEOMETRY installed CPU emitter at eboot+0x1a1960\n", stderr);
    return 1;
}
