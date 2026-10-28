// SPDX-License-Identifier: GPL-2.0-or-later
#include "feedback_arena.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool iss::native::InitializeArena(std::uint64_t* arena, std::uint64_t base,
                                  std::int32_t length) {
    if (arena[1] != 0) return false;
    arena[0] = base;
    arena[1] = base;
    arena[2] = base + static_cast<std::uint64_t>(static_cast<std::int64_t>(length));
    return true;
}

namespace {
// Entire original function, 25 bytes; SHA256 e8f64174a5bb318f64218cff4a7fcb8874833661f56e0d232be3fb9720af32d1.
constexpr std::array<unsigned char, 25> signature{
    0x48,0x83,0x7f,0x08,0x00,0x75,0x11,0x48,0x63,0xc2,0x48,0x89,0x37,
    0x48,0x89,0x77,0x08,0x48,0x01,0xf0,0x48,0x89,0x47,0x10,0xc3};
std::uint64_t* feedback_arena{};
iss::native::ArenaSink sink{};
std::atomic<std::uint64_t> epoch{};
void __attribute__((sysv_abi)) Initialize(std::uint64_t* arena, std::uint64_t base,
                                         std::int32_t length) {
    const bool initialized = iss::native::InitializeArena(arena, base, length);
    if (initialized && arena == feedback_arena && base && length == 0x1e00000 && sink) {
        // This observes arena activation, not retirement of any old GPU/CPU alias.
        sink(base, static_cast<std::uint64_t>(length), ++epoch);
    }
}
}

int iss::native::InstallFeedbackArena(unsigned char* image, std::size_t size,
                                       ArenaSink callback) {
    const char* option = std::getenv("ISS_FEEDBACK_ARENA");
    if (!option || option[0] != '1') return 0;
    constexpr std::size_t entry = 0x577cb0, context = 0x7d3eb18;
    if (!image || !callback || size < context + 24 ||
        std::memcmp(image + entry, signature.data(), signature.size())) {
        std::fputs("FEEDBACK_ARENA rejected: function identity or callback missing\n", stderr);
        return -1;
    }
    std::array<unsigned char, 14> patch{0xff,0x25,0,0,0,0};
    const auto target = reinterpret_cast<std::uintptr_t>(&Initialize);
    std::memcpy(patch.data() + 6, &target, sizeof(target));
    DWORD old{}, ignored{};
    if (!VirtualProtect(image + entry, patch.size(), PAGE_EXECUTE_READWRITE, &old)) return -1;
    feedback_arena = reinterpret_cast<std::uint64_t*>(image + context);
    sink = callback;
    std::memcpy(image + entry, patch.data(), patch.size());
    if (!VirtualProtect(image + entry, patch.size(), old, &ignored) ||
        !FlushInstructionCache(GetCurrentProcess(), image + entry, patch.size())) return -1;
    std::fputs("FEEDBACK_ARENA native initializer installed; epochs are observational\n", stderr);
    return 1;
}
