#include <cassert>

#include "infamous_renderer/generated_feedback_candidates.h"

int main() {
    using namespace iss::game::feedback;
    static_assert(AtomicCandidates.size() == 351);
    static_assert(FindAtomicCandidate(0x3EA96DB0)->known_output_stride_bytes == 64);
    static_assert(FindAtomicCandidate(0x63E538C1)->known_output_stride_bytes == 80);
    static_assert(FindAtomicCandidate(0x9C58AA06)->known_output_stride_bytes == 112);
    assert(FindAtomicCandidate(0) == nullptr);
    bool has_word10 = false;
    for (const auto& item : AtomicCandidates) {
        assert(item.atomic_word_mask & (1 << 1));
        has_word10 |= bool(item.atomic_word_mask & (1 << 10));
    }
    assert(has_word10);
}
