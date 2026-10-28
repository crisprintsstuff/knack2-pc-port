// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>

#include "g4_cp_coordination.h"

using iss::g4::CpCoordination;
using iss::g4::CpLane;
using iss::g4::CpTick;
using iss::g4::QidTick;
using iss::g4::SealState;

namespace {

void FrameMarksRequireEveryQidPrefix() {
    CpCoordination<57> cp;
    std::uint64_t gen1 = 0, gen2 = 0;
    assert(cp.Map(1, gen1) && cp.Map(2, gen2));

    CpCoordination<57>::SubmissionToken gfx{}, asc1{}, asc2{};
    assert(cp.Issue(0, gfx) && cp.Issue(1, asc1));
    const auto old_mark = cp.CaptureFrame(10);
    assert(old_mark && old_mark->targets[0].sequence == 1 &&
           old_mark->targets[1].sequence == 1 && old_mark->targets[2].sequence == 0);
    assert(cp.Issue(2, asc2));
    assert(cp.CaptureFrame(11));

    assert(cp.Complete(asc2));
    assert(!cp.FrontFrameReady());
    assert(!cp.RetireFrontFrame());
    // A global completed count now reaches the old mark's issue count, but G is blocked.
    assert(cp.Complete(asc1) && !cp.FrontFrameReady());
    assert(cp.Complete(gfx));
    assert(cp.FrontFrameReady());
    assert(cp.RetireFrontFrame()->id == 10);
    assert(cp.RetireFrontFrame()->id == 11);

    const auto remap_mark = cp.CaptureFrame(12);
    assert(remap_mark);
    std::uint64_t closing = 0, new_gen = 0;
    assert(cp.BeginClose(1, closing) && closing == gen1);
    assert(cp.Drained(1, gen1) && cp.FinishClose(1, gen1));
    assert(cp.Map(1, new_gen) && new_gen > gen1);
    assert(cp.FrontFrameReady()); // old generation was drained before slot reuse
    assert(cp.RetireFrontFrame()->id == 12);

    CpCoordination<57> asc_order;
    std::uint64_t a = 0, b = 0;
    assert(asc_order.Map(1, a) && asc_order.Map(2, b));
    CpCoordination<57>::SubmissionToken first{}, second{};
    assert(asc_order.Issue(1, first) && asc_order.Issue(2, second));
    assert(asc_order.CaptureFrame(13));
    assert(asc_order.Complete(second));
    assert(!asc_order.FrontFrameReady()); // qid2 cannot substitute for qid1's prefix
    assert(asc_order.Complete(first) && asc_order.FrontFrameReady());
}

void FrameMarkWindowIsBounded() {
    CpCoordination<57> cp;
    for (std::uint64_t i = 0; i < 16; ++i) assert(cp.CaptureFrame(i));
    assert(!cp.CaptureFrame(16));
    for (std::uint64_t i = 0; i < 16; ++i) {
        assert(cp.FrontFrameReady());
        assert(cp.RetireFrontFrame()->id == i);
    }
    assert(cp.CaptureFrame(16));
}

void SealWaitsForRecordAndActualSubmit() {
    CpCoordination<57> cp;
    std::uint64_t asc_gen = 0;
    assert(cp.Map(1, asc_gen));
    const QidTick gfx{CpLane::Graphics, 0, cp.Generation(0), 1};
    const QidTick asc{CpLane::Asc, 1, asc_gen, 1};

    assert(cp.BeginRecord(CpLane::Graphics, 1));
    assert(cp.BeginRecord(CpLane::Asc, 1));
    const auto to_gfx = cp.SealAtLeast(gfx);
    const auto to_asc = cp.SealAtLeast(asc);
    assert(to_gfx->State() == SealState::Pending && to_asc->State() == SealState::Pending);
    assert(!cp.PlanSeal(CpLane::Graphics, false));
    assert(!cp.PlanSeal(CpLane::Asc, false));

    assert(cp.EndRecord(CpLane::Graphics, 1));
    assert(cp.EndRecord(CpLane::Asc, 1));
    assert(!cp.PlanSeal(CpLane::Graphics, true)); // no submit under a cache lock
    const auto gplan = cp.PlanSeal(CpLane::Graphics, false);
    const auto cplan = cp.PlanSeal(CpLane::Asc, false);
    assert(gplan && cplan);
    assert(!cp.BeginRecord(CpLane::Graphics, 1)); // sealing pins the open tick
    assert(to_gfx->State() == SealState::Pending && to_asc->State() == SealState::Pending);
    assert(cp.FinishSeal(*gplan, true));
    assert(cp.FinishSeal(*cplan, true));
    assert(to_gfx->State() == SealState::Done && to_gfx->SealedTick() == 1);
    assert(to_asc->State() == SealState::Done && to_asc->SealedTick() == 1);
    assert(cp.OpenTick(CpLane::Graphics) == 2 && cp.OpenTick(CpLane::Asc) == 2);

    // Both owners can now prepare consumers of each other's already sealed prefixes.
    assert(cp.AddDependency(CpTick{CpLane::Graphics, 2}, asc));
    assert(cp.AddDependency(CpTick{CpLane::Asc, 2}, gfx));
    assert(cp.ActiveDependencies() == 0); // sealed producers need no open DAG edge
}

void MutualOpenTickWaitIsRejected() {
    CpCoordination<57> cp;
    std::uint64_t asc_gen = 0;
    assert(cp.Map(1, asc_gen));
    const QidTick gfx{CpLane::Graphics, 0, cp.Generation(0), 1};
    const QidTick asc{CpLane::Asc, 1, asc_gen, 1};
    assert(cp.AddDependency(CpTick{CpLane::Graphics, 1}, asc));
    assert(!cp.AddDependency(CpTick{CpLane::Asc, 1}, gfx));
    assert(cp.ActiveDependencies() == 1);
    assert(!cp.BeginRecord(CpLane::Graphics, 1)); // producer's open tick is not sealed
    const auto gfx_seal = cp.SealAtLeast(gfx);
    assert(!cp.PlanSeal(CpLane::Graphics, false)); // cannot seal a blocked consumer tick
    assert(!cp.PublishSubmitted(CpLane::Graphics, 1));
    assert(cp.PublishSubmitted(CpLane::Asc, 1));
    assert(cp.ActiveDependencies() == 0);
    // The consumer can record only after its producer is sealed.
    assert(cp.BeginRecord(CpLane::Graphics, 1));
    assert(!cp.AddDependency(CpTick{CpLane::Graphics, 1}, asc));
    assert(!cp.PlanSeal(CpLane::Graphics, false));
    assert(cp.EndRecord(CpLane::Graphics, 1));
    const auto plan = cp.PlanSeal(CpLane::Graphics, false);
    assert(plan && cp.FinishSeal(*plan, true));
    assert(gfx_seal->State() == SealState::Done);

    CpCoordination<57> retry;
    assert(retry.Map(1, asc_gen));
    assert(retry.AddDependency(CpTick{CpLane::Graphics, 1},
                               QidTick{CpLane::Asc, 1, asc_gen, 1}));
    retry.CancelConsumer(CpTick{CpLane::Graphics, 1});
    assert(retry.ActiveDependencies() == 0);
    assert(retry.AddDependency(CpTick{CpLane::Asc, 1},
                               QidTick{CpLane::Graphics, 0, retry.Generation(0), 1}));
}

void OrdinarySubmitAlsoAcknowledgesSeals() {
    CpCoordination<57> cp;
    const QidTick gfx{CpLane::Graphics, 0, cp.Generation(0), 1};
    const auto request = cp.SealAtLeast(gfx);
    assert(request->State() == SealState::Pending);
    assert(cp.BeginRecord(CpLane::Graphics, 1));
    assert(!cp.PublishSubmitted(CpLane::Graphics, 1));
    assert(request->State() == SealState::Pending);
    assert(cp.EndRecord(CpLane::Graphics, 1));
    assert(cp.PublishSubmitted(CpLane::Graphics, 1));
    assert(request->State() == SealState::Done && request->SealedTick() == 1);
}

void FailureStopAndGenerationDoNotAcknowledge() {
    CpCoordination<57> cp;
    std::uint64_t gen = 0;
    assert(cp.Map(1, gen));
    const QidTick asc{CpLane::Asc, 1, gen, 1};
    const auto failed = cp.SealAtLeast(asc);
    const auto plan = cp.PlanSeal(CpLane::Asc, false);
    assert(plan && failed->State() == SealState::Pending);
    assert(!cp.FinishSeal(*plan, false));
    assert(failed->State() == SealState::Failed);
    const auto cancelled = cp.SealAtLeast(asc);
    cp.Stop();
    assert(cancelled->State() == SealState::Cancelled);
    assert(cp.SealAtLeast(asc)->State() == SealState::Cancelled);

    CpCoordination<57> remapped;
    std::uint64_t old_gen = 0, new_gen = 0, closing = 0;
    assert(remapped.Map(1, old_gen));
    const auto old_waiter = remapped.SealAtLeast(QidTick{CpLane::Asc, 1, old_gen, 1});
    assert(old_waiter->State() == SealState::Pending);
    assert(remapped.BeginClose(1, closing));
    assert(remapped.FinishClose(1, closing));
    assert(old_waiter->State() == SealState::Failed);
    assert(remapped.Map(1, new_gen) && new_gen != old_gen);
    assert(remapped.SealAtLeast(QidTick{CpLane::Asc, 1, old_gen, 1})->State() ==
           SealState::Failed);
}

void GuestAscSealPinsTheGlobalOpenTick() {
    CpCoordination<57> cp;
    assert(cp.BeginRecord(CpLane::Asc, 1));
    const auto guest = cp.SealGuestAscPrefix();
    assert(guest->target.lane == CpLane::Asc && guest->target.qid == 0 &&
           guest->target.generation == 0 && guest->target.tick == 1);
    assert(guest->State() == SealState::Pending);
    assert(!cp.PlanSeal(CpLane::Asc, false));
    assert(cp.EndRecord(CpLane::Asc, 1));
    const auto plan = cp.PlanSeal(CpLane::Asc, false);
    assert(plan && plan->through_tick == 1);
    assert(!cp.BeginRecord(CpLane::Asc, 1));
    assert(cp.FinishSeal(*plan, true));
    assert(guest->State() == SealState::Done && guest->SealedTick() == 1);
    assert(cp.OpenTick(CpLane::Asc) == 2);

    const auto next = cp.SealGuestAscPrefix();
    cp.Stop();
    assert(next->State() == SealState::Cancelled);
}

} // namespace

int main() {
    FrameMarksRequireEveryQidPrefix();
    FrameMarkWindowIsBounded();
    SealWaitsForRecordAndActualSubmit();
    MutualOpenTickWaitIsRejected();
    OrdinarySubmitAlsoAcknowledgesSeals();
    FailureStopAndGenerationDoNotAcknowledge();
    GuestAscSealPinsTheGlobalOpenTick();
}
