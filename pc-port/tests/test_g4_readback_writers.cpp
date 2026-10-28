// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cassert>
#include <memory>

#include "g4_graphics_control.h"
#include "g4_readback_writers.h"

using namespace iss::g4;

int main() {
    ReadbackWriterLedger<2> ledger;
    ledger.Publish({{CpLane::Graphics, 0, 1, 8}, 0x1000, 8, 7, 11});
    ledger.Publish({{CpLane::Asc, 2, 4, 13}, 0x1000, 8, 7, 12});
    const std::array<std::uint64_t, 2> ticks{8, 13};
    const auto both = ledger.Capture(0x1000, 8, 7, 12, ticks);
    assert(both && both->producers[0].qid == 0 && both->producers[1].qid == 2);
    assert(ledger.StillCurrent(*both, 7, 12, ticks));

    // A queued G control owns the snapshot; a C writer published before its
    // safe point makes the result stale instead of authorizing old bytes.
    GraphicsControlMailbox mailbox;
    auto valid = std::make_shared<std::atomic<bool>>(true);
    const auto control = mailbox.Enqueue([snapshot = *both, &ledger, valid] {
        valid->store(ledger.StillCurrent(snapshot, 7, 13, {8, 13}),
                     std::memory_order_release);
    });
    assert(control && control->Status() == GraphicsControlMailbox::Control::State::Queued);
    assert(!mailbox.ServeOne(false));
    assert(mailbox.ServeOne(true));
    assert(control->Status() == GraphicsControlMailbox::Control::State::Done);
    assert(!valid->load(std::memory_order_acquire));

    assert(!ledger.StillCurrent(*both, 7, 13, ticks)); // newer writer during copy
    assert(!ledger.StillCurrent(*both, 8, 12, ticks)); // remap during copy

    ledger.Invalidate(0x1004, 1); // CPU write through overlapping bytes
    assert(!ledger.Capture(0x1000, 8, 7, 12, ticks));

    ledger.Publish({{CpLane::Graphics, 0, 1, 9}, 0x2000, 8, 7, 13});
    ledger.Publish({{CpLane::Asc, 3, 5, 14}, 0x2000, 8, 7, 14});
    const auto newer = ledger.Capture(0x2000, 8, 7, 14, {9, 14});
    assert(newer && newer->producers[1].qid == 3);
    // Bounded history evicted the older writer: absence of provenance is a miss.
    assert(!ledger.Capture(0x1000, 8, 7, 12, ticks));
}
