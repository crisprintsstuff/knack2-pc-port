// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace iss::native {
// Exact memory semantics of EBOOT 0x577CB0. Length is sign-extended from EDX.
// Returns whether initialization occurred; no epoch is inferred from bank parity.
bool InitializeArena(std::uint64_t* arena, std::uint64_t base, std::int32_t length);
using ArenaSink = void (*)(std::uint64_t base, std::uint64_t size, std::uint64_t epoch);
int InstallFeedbackArena(unsigned char* image, std::size_t size, ArenaSink sink);
}
