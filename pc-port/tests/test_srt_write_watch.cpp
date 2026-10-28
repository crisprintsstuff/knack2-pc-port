// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "srt_write_watch.h"

using iss::g4::SrtWriteWatchState;

int main() {
    SrtWriteWatchState page;
    std::uint64_t a{}, b{}, c{};
    assert(page.Arm(0, a));
    assert(page.Arm(1, b));
    assert(a == b && page.Check(a));
    assert(page.Invalidate() == 2); // first CPU write removes both protections
    assert(!page.Check(a));
    assert(page.Invalidate() == 0); // second write does not decrement again
    assert(page.Arm(0, c));
    assert(c != a && page.Check(c));
    assert(!page.Release(a));       // old packet cannot remove new packet's protection
    assert(page.Check(c));
    assert(page.Release(c));
    assert(!page.Check(c));

    SrtWriteWatchState remapped;
    std::uint64_t old{}, next{};
    assert(remapped.Arm(0, old));
    assert(remapped.Invalidate() == 1); // unmap/remap invalidates generation
    assert(remapped.Arm(0, next) && next != old);
    assert(!remapped.Release(old) && remapped.Check(next));

    SrtWriteWatchState full;
    std::uint64_t rejected{};
    assert(!full.Arm(255, rejected));
    assert(!full.Check(rejected));
}
