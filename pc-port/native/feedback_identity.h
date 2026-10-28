// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace iss::native {

using FeedbackIdentitySink = void (*)(std::uint64_t owner_object,
                                      std::uint64_t shader_object,
                                      std::uint64_t record_address);

// Replaces only the known eboot CALL instruction. The original callee remains
// intact; false means the guest code is unchanged and the generic GPU path runs.
bool InstallFeedbackIdentityCall(unsigned char* image, std::size_t image_size,
                                 FeedbackIdentitySink sink) noexcept;

} // namespace iss::native

extern "C" {
extern std::uintptr_t iss_feedback_identity_target;
void iss_feedback_identity_stub();
}
