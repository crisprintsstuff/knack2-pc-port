// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>

#include "g4_sparse_residency.h"

using iss::g4::SparseResidencyPublication;

int main() {
    SparseResidencyPublication<int> published;
    std::uint64_t next_tick = 1;
    const auto reserve = [&] { return next_tick++; };

    // Two arenas prepared in one batch require one immutable bind tick.
    published.NoteBind(10, reserve);
    published.NoteBind(20, reserve);
    assert(next_tick == 2);
    assert(!published.RequiredSubmittedTick(10));
    assert(!published.RequiredSubmittedTick(20));
    const auto first = published.TakePendingTick();
    assert(first == 1 && published.PendingTick() == 0);
    // Removing the pending batch must not make a consumer see a ready arena.
    assert(!published.RequiredSubmittedTick(10));
    published.PublishSubmitted(first);
    assert(published.RequiredSubmittedTick(10) == first);
    assert(published.RequiredSubmittedTick(20) == first);

    // A new bind on arena 10 hides its old submitted ticket until the new
    // bindSparse is submitted. Arena 20 remains usable on its earlier ticket.
    published.NoteBind(10, reserve);
    assert(!published.RequiredSubmittedTick(10));
    assert(published.RequiredSubmittedTick(20) == first);
    const auto second = published.TakePendingTick();
    assert(second == 2);
    assert(!published.RequiredSubmittedTick(10));
    published.PublishSubmitted(second);
    assert(published.RequiredSubmittedTick(10) == second);
    assert(published.RequiredSubmittedTick(20) == first);
}
