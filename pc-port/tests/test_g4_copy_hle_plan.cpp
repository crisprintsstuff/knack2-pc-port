#include <array>
#include <cassert>
#include <cstdint>

#include "g4_copy_hle_plan.h"

using namespace iss::g4;

int main() {
    constexpr std::array controls{
        CopyHleControl{2, 1, 1}, CopyHleControl{4, 3, 0},
        CopyHleControl{70'000'000, 70'000'000, 0}};
    const auto plan = BuildCopyHlePlan(controls, 1, 80'000'000, 80'000'000);
    assert(plan && plan->copies.size() == 3 && plan->batches.size() == 2);
    assert(plan->copies[0].src_offset == 1 && plan->copies[0].dst_offset == 2 &&
           plan->copies[0].size == 2);
    assert(plan->batches[0].first == 0 && plan->batches[0].count == 2);
    assert(plan->batches[1].first == 2 && plan->batches[1].count == 1);
    auto changed = controls;
    auto owned = BuildCopyHlePlan(changed, 1, 80'000'000, 80'000'000);
    assert(owned);
    changed[0].src_index = 999;
    assert(owned->copies[0].src_offset == 1); // no pointer into mutable control RAM
    constexpr std::array bad{CopyHleControl{100, 0, 0}};
    assert(!BuildCopyHlePlan(bad, 4, 1000, 400));
    assert(!BuildCopyHlePlan(controls, 0, 80'000'000, 80'000'000));
    constexpr std::array huge{CopyHleControl{0, 0, UINT32_MAX}};
    assert(!BuildCopyHlePlan(huge, UINT32_MAX, UINT64_MAX / 2, UINT64_MAX));
}
