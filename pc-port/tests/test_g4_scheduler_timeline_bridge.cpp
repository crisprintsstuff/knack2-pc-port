// SPDX-License-Identifier: GPL-2.0-or-later
// Post-submit notifications are supplied explicitly; no sleeps or scheduler threads.
#include <cassert>
#include <cstdint>

#include "g4_cp_coordination.h"
#include "g4_scheduler_timeline_bridge.h"

using namespace iss::g4;

int main() {
    CpCoordination<57> cp;
    SchedulerTimelineBridge bridge{cp};
    std::uint64_t asc_generation{};
    assert(cp.Map(1, asc_generation));
    assert(!bridge.ObserveSubmitted(CpLane::Graphics, 0));

    // Ordinary graphics Flush/Finish calls may submit several ticks before a
    // fallback. Every earlier tick is a submitted prefix of the same timeline.
    assert(bridge.ObserveSubmitted(CpLane::Graphics, 1));
    assert(bridge.ObserveSubmitted(CpLane::Graphics, 3));
    assert(cp.SubmittedTick(CpLane::Graphics) == 3);
    assert(cp.OpenTick(CpLane::Graphics) == 4);
    assert(cp.OpenTick(CpLane::Asc) == 1);

    const QidTick graphics{CpLane::Graphics, 0, cp.Generation(0), 4};
    auto seal = cp.SealAtLeast(graphics);
    assert(seal->State() == SealState::Pending);
    const auto plan = cp.PlanSeal(CpLane::Graphics, false);
    assert(plan && plan->through_tick == 4);
    // The Scheduler callback arrives after queue.submit, but a pinned seal
    // still owns tick 4. No ACK occurs until FinishSeal releases that pin.
    assert(!bridge.ObserveSubmitted(CpLane::Graphics, 4));
    assert(seal->State() == SealState::Pending);
    assert(cp.FinishSeal(*plan, true));
    assert(bridge.Reconcile(CpLane::Graphics));
    assert(seal->State() == SealState::Done && seal->SealedTick() == 4);
    assert(cp.OpenTick(CpLane::Graphics) == 5);
    assert(bridge.ObserveSubmitted(CpLane::Graphics, 2)); // old callback cannot rewind
    assert(cp.OpenTick(CpLane::Graphics) == 5);

    assert(bridge.ObserveSubmitted(CpLane::Asc, 1));
    assert(cp.OpenTick(CpLane::Asc) == 2);
    assert(cp.AddDependency(CpTick{CpLane::Asc, 2}, graphics));
    assert(cp.BeginRecord(CpLane::Asc, 2));
    assert(cp.EndRecord(CpLane::Asc, 2));
}
