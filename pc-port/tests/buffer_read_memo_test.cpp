// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/buffer_read_memo.h"
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

using iss::buffers::SmallReadMemo;
using iss::buffers::SmallReadStamp;
using Memo = SmallReadMemo<std::uintptr_t, 8>;

static SmallReadStamp Initial(std::uint64_t address = 0x10000) {
    return {address, 64, 1, 1, 7, 9, 11, 13, {17, 19}, 0, 8};
}

int main() {
    Memo memo;
    auto original = Initial();
    assert(!memo.Find(original));
    assert(memo.Remember(original, original, 0x100, 64));
    assert(memo.Find(original)->handle == 0x100);
    assert(memo.Find(original)->offset == 64);
    unsigned invalidations{};
    const auto invalidate = [&](SmallReadStamp changed) {
        assert(changed != original);
        assert(!memo.Find(changed));
        assert(!memo.Remember(original, changed, 0x200, 128));
        assert(memo.Find(original)->handle == 0x100);
        ++invalidations;
    };
    auto changed = original; ++changed.address; invalidate(changed);
    changed = original; ++changed.size; invalidate(changed);
    changed = original; ++changed.mapping_generation; invalidate(changed);
    changed = original; ++changed.arena_generation; invalidate(changed);
    changed = original; ++changed.gpu_write_sequence; invalidate(changed);
    changed = original; ++changed.upload_epoch; invalidate(changed);
    changed = original; ++changed.host_write_generation; invalidate(changed);
    changed = original; ++changed.scheduler_tick; invalidate(changed);
    changed = original; ++changed.cpu_versions[0]; invalidate(changed);
    changed = original; ++changed.cpu_versions[1]; invalidate(changed);
    changed = original; ++changed.queue; invalidate(changed);
    changed = original; ++changed.stable_frames; invalidate(changed);
    assert(!memo.Remember(original, original, 0, 64));
    auto empty = original; empty.size = 0;
    assert(!memo.Remember(empty, empty, 0x100, 64));
    empty = original; empty.address = 0;
    assert(!memo.Remember(empty, empty, 0x100, 64));

    // Force collisions without access to the private hash. A collision may
    // evict an observation, never return another address/range/handle.
    std::vector<SmallReadStamp> inserted;
    for(unsigned i = 0; i < 128; ++i) {
        auto stamp = Initial(0x10000 + 4096 * i + (i & 15));
        stamp.size = 1 + i;
        assert(memo.Remember(stamp, stamp, 0x100 + i, i * 16));
        inserted.push_back(stamp);
        for(unsigned j = 0; j < inserted.size(); ++j) {
            if(const auto* hit = memo.Find(inserted[j])) {
                assert(hit->handle == 0x100 + j);
                assert(hit->offset == j * 16);
                assert(hit->stamp == inserted[j]);
            }
        }
    }

    // Differential event trace. The uncached path chooses an arena only when
    // CPU-clean and hazard-free. The candidate repeats the live hazard query;
    // a same-tick upload MUST fall back to the stream route even if all stamp
    // fields match. Mapping/arena changes switch the expected Vulkan handle.
    struct Resource {
        SmallReadStamp stamp;
        std::uintptr_t arena;
        std::uint64_t offset;
        bool clean{true}, hazard{};
    };
    std::vector<Resource> resources;
    for(unsigned i = 0; i < 24; ++i)
        resources.push_back({Initial(0x10000 + 4096 * i), 0x100 + i, i * 64});
    SmallReadMemo<std::uintptr_t, 32> candidate;
    unsigned generic{}, reused{}, hazard_fallbacks{}, unstable_stores{};
    const auto reference = [](const Resource& resource) {
        // Deterministic stand-ins for the original arena/stream binding result.
        return resource.clean && !resource.hazard
            ? std::pair(resource.arena, resource.offset)
            : std::pair(std::uintptr_t{0x8000}, resource.stamp.address);
    };
    const auto bind = [&](Resource& resource, bool race) {
        const auto expected = reference(resource);
        const auto before = resource.stamp;
        if(const auto* entry = candidate.Find(before); entry && !resource.hazard) {
            assert(resource.clean); // Any CPU dirtiness must have changed versions.
            ++reused;
            assert(std::pair(entry->handle, entry->offset) == expected);
            return;
        } else if(entry && resource.hazard) {
            ++hazard_fallbacks;
        }
        ++generic;
        if(resource.clean && !resource.hazard) {
            if(race) ++resource.stamp.cpu_versions[0];
            const bool stored = candidate.Remember(before, resource.stamp,
                                                  resource.arena, resource.offset);
            assert(stored == !race);
            unstable_stores += !stored;
        }
    };
    // Deliberately produce a matching stamp followed by a hazard without any
    // generation change, the case an unsafe cached known_unwritten would miss.
    bind(resources[0], false); bind(resources[0], false);
    resources[0].hazard = true; bind(resources[0], false);
    assert(hazard_fallbacks == 1);
    resources[0].hazard = false;
    std::uint32_t random = 0x613eadf2;
    for(unsigned n = 0; n < 60000; ++n) {
        random = random * 1664525 + 1013904223;
        auto& resource = resources[(random >> 20) % resources.size()];
        switch((random >> 12) & 15) {
        case 0: ++resource.stamp.cpu_versions[0]; resource.clean = false; break;
        case 1: ++resource.stamp.cpu_versions[1]; resource.clean = false; break;
        case 2: ++resource.stamp.gpu_write_sequence; break;
        case 3: ++resource.stamp.upload_epoch; break;
        case 4: ++resource.stamp.host_write_generation; break;
        case 5: ++resource.stamp.mapping_generation; resource.arena += 0x100; break;
        case 6: ++resource.stamp.arena_generation; resource.arena += 0x100; break;
        case 7: ++resource.stamp.scheduler_tick; break;
        case 8: resource.hazard = !resource.hazard; break;
        case 9: resource.clean = true; break;
        default: break;
        }
        bind(resource, n % 101 == 0);
    }
    assert(reused && generic && hazard_fallbacks && unstable_stores);
    std::printf("PASS: %u stamp invalidations, collision checks, 60003 differential bindings "
                "(%u reused, %u live-hazard fallbacks, %u interrupted proofs).\n",
                invalidations, reused, hazard_fallbacks, unstable_stores);
}
