// SPDX-License-Identifier: GPL-2.0-or-later
#include "feedback_identity.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
std::uintptr_t iss_feedback_identity_target{};
}

namespace {
iss::native::FeedbackIdentitySink identity_sink{};
constexpr std::size_t kCallsite = 0x3F070C;
constexpr std::size_t kSignatureStart = 0x3F06F1;
constexpr std::size_t kCallee = 0x3FAB70;
// Ghidra /eboot.elf (SHA-256 b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605).
// Ends immediately after CALL 0x3FAB70, before the next CMP.
constexpr std::array<unsigned char, 32> kCallsiteSignature{
    0x4C,0x89,0xFF,0x4D,0x89,0xE8,0x49,0x89,0xD9,0x48,0x89,0x44,0x24,0x10,
    0x48,0x89,0x4C,0x24,0x08,0x48,0x8D,0x4D,0xA0,0x4C,0x89,0x34,0x24,
    0xE8,0x5F,0xA4,0x00,0x00,
};
constexpr std::array<unsigned char, 17> kCalleeSignature{
    0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
    0x48,0x83,0xEC,0x58,
};
constexpr std::size_t kBridgeSize = 14;

bool FitsRel32(std::uintptr_t next, std::uintptr_t target) noexcept {
    if (target >= next) return target - next <= static_cast<std::uintptr_t>(INT32_MAX);
    return next - target <= static_cast<std::uintptr_t>(-static_cast<std::int64_t>(INT32_MIN));
}

std::uintptr_t AlignUp(std::uintptr_t value, std::uintptr_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

void* AllocateNear(std::uintptr_t next) noexcept {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const auto granularity = static_cast<std::uintptr_t>(sys.dwAllocationGranularity);
    if (!granularity || (granularity & (granularity - 1))) return nullptr;
    constexpr std::uintptr_t kPage = 4096;
    const auto lo = next > static_cast<std::uintptr_t>(INT32_MAX)
                        ? next - static_cast<std::uintptr_t>(INT32_MAX) : granularity;
    const auto hi = next <= std::numeric_limits<std::uintptr_t>::max() -
                                static_cast<std::uintptr_t>(INT32_MAX)
                        ? next + static_cast<std::uintptr_t>(INT32_MAX)
                        : std::numeric_limits<std::uintptr_t>::max();
    for (auto cursor = std::max(lo, granularity); cursor < hi;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) break;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto end = info.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - base
                             ? base + info.RegionSize : std::numeric_limits<std::uintptr_t>::max();
        if (info.State == MEM_FREE) {
            const auto candidate = AlignUp(std::max(cursor, base), granularity);
            if (candidate >= base && candidate <= end && end - candidate >= kPage &&
                FitsRel32(next, candidate)) {
                if (void* page = VirtualAlloc(reinterpret_cast<void*>(candidate), kPage,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return page;
            }
        }
        if (end <= cursor) break;
        cursor = end;
    }
    return nullptr;
}
} // namespace

extern "C" void iss_feedback_identity_bridge(std::uint64_t owner_object,
                                               std::uint64_t shader_object,
                                               std::uint64_t record_address) noexcept {
    if (identity_sink) identity_sink(owner_object, shader_object, record_address);
}

bool iss::native::InstallFeedbackIdentityCall(unsigned char* image, std::size_t image_size,
                                              FeedbackIdentitySink sink) noexcept {
    if (!image || !sink || image_size < kSignatureStart + kCallsiteSignature.size() ||
        image_size < kCallee + kCalleeSignature.size() ||
        std::memcmp(image + kSignatureStart, kCallsiteSignature.data(), kCallsiteSignature.size()) ||
        std::memcmp(image + kCallee, kCalleeSignature.data(), kCalleeSignature.size())) {
        std::fputs("FEEDBACK_IDENTITY disabled: eboot signature mismatch\n", stderr);
        return false;
    }
    auto* const site = image + kCallsite;
    const auto next = reinterpret_cast<std::uintptr_t>(site + 5);
    void* bridge = AllocateNear(next);
    if (!bridge) {
        std::fputs("FEEDBACK_IDENTITY disabled: no rel32 bridge near eboot\n", stderr);
        return false;
    }
    std::array<unsigned char, kBridgeSize> jump{0xFF, 0x25, 0, 0, 0, 0};
    const auto stub = reinterpret_cast<std::uintptr_t>(&iss_feedback_identity_stub);
    std::memcpy(jump.data() + 6, &stub, sizeof(stub));
    std::memcpy(bridge, jump.data(), jump.size());
    DWORD old_bridge{};
    if (!VirtualProtect(bridge, 4096, PAGE_EXECUTE_READ, &old_bridge) ||
        !FlushInstructionCache(GetCurrentProcess(), bridge, jump.size())) {
        VirtualFree(bridge, 0, MEM_RELEASE);
        std::fputs("FEEDBACK_IDENTITY disabled: bridge protection failed\n", stderr);
        return false;
    }

    std::array<unsigned char, 5> patch{0xE8};
    const auto bridge_address = reinterpret_cast<std::uintptr_t>(bridge);
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(bridge_address) - static_cast<std::int64_t>(next));
    std::memcpy(patch.data() + 1, &displacement, sizeof(displacement));
    DWORD old_site{};
    if (!VirtualProtect(site, patch.size(), PAGE_EXECUTE_READWRITE, &old_site)) {
        VirtualFree(bridge, 0, MEM_RELEASE);
        std::fputs("FEEDBACK_IDENTITY disabled: callsite protection failed\n", stderr);
        return false;
    }
    // The guest has not entered module initializers yet; no guest thread can
    // execute this five-byte call while it is replaced.
    iss_feedback_identity_target = reinterpret_cast<std::uintptr_t>(image + kCallee);
    identity_sink = sink;
    std::memcpy(site, patch.data(), patch.size());
    DWORD ignored{};
    const bool restored = VirtualProtect(site, patch.size(), old_site, &ignored) != 0;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), site, patch.size()) != 0;
    if (!restored || !flushed) {
        DWORD writable{};
        bool rolled_back=false;
        if (VirtualProtect(site, patch.size(), PAGE_EXECUTE_READWRITE, &writable)) {
            std::memcpy(site, kCallsiteSignature.data() + (kCallsite - kSignatureStart), patch.size());
            const bool protected_again=VirtualProtect(site, patch.size(), old_site, &ignored)!=0;
            const bool cache_flushed=FlushInstructionCache(GetCurrentProcess(), site, patch.size())!=0;
            rolled_back=protected_again&&cache_flushed&&
                        !std::memcmp(site,kCallsiteSignature.data()+
                                          (kCallsite-kSignatureStart),patch.size());
        }
        if(!rolled_back){
            // A live patched CALL must never point at a freed bridge. Guest
            // startup has not begun; fail this process instead of continuing.
            std::fputs("FEEDBACK_IDENTITY fatal: callsite rollback failed\n",stderr);
            std::abort();
        }
        identity_sink = nullptr;
        iss_feedback_identity_target = 0;
        VirtualFree(bridge, 0, MEM_RELEASE);
        std::fputs("FEEDBACK_IDENTITY disabled: callsite patch rollback\n", stderr);
        return false;
    }
    std::fprintf(stderr, "FEEDBACK_IDENTITY installed at eboot+0x3F070C bridge=%p\n", bridge);
    return true;
}
