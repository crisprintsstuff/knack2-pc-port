// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>

#include "g4_texture_negative_index.h"

using namespace iss::g4;

int main() {
    TextureNegativeIndex index;
    std::array<std::atomic<std::uint32_t>, 8> page_counts{};
    const auto count = [&](std::uint64_t page) {
        return page_counts[static_cast<std::size_t>(page)].load(std::memory_order_acquire);
    };

    const auto empty = index.Probe(0x1100, 0x1800, 12, page_counts.size(), count);
    assert(empty.result == NegativeImageResult::NoRegisteredPage);
    assert(index.StillCurrent(empty));
    assert(index.Probe(0x1000, 0, 12, page_counts.size(), count).result ==
           NegativeImageResult::Unknown);
    assert(index.Probe(UINT64_MAX, 2, 12, page_counts.size(), count).result ==
           NegativeImageResult::Unknown);

    index.BeginRegistryMutation();
    assert(index.Probe(0x1100, 0x1800, 12, page_counts.size(), count).result ==
           NegativeImageResult::Unknown);
    page_counts[2].store(1, std::memory_order_release);
    index.EndRegistryMutation();
    assert(!index.StillCurrent(empty));
    const auto hit = index.Probe(0x1100, 0x1800, 12, page_counts.size(), count);
    assert(hit.result == NegativeImageResult::HasRegisteredPage);

    index.BeginRegistryMutation();
    page_counts[2].store(0, std::memory_order_release);
    index.EndRegistryMutation();
    const auto gone = index.Probe(0x1100, 0x1800, 12, page_counts.size(), count);
    assert(gone.result == NegativeImageResult::NoRegisteredPage);
    assert(index.StillCurrent(gone));
}
