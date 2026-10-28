// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic owner handoff exercised without scheduler timing or a game process.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "g4_cp_coordination.h"
#include "g4_fallback_packet.h"
#include "srt_walker_capture.h"

using namespace iss::g4;

namespace {

struct TestLease final : SrtReadWatchLease {
    bool Covers(std::span<const SrtReadSnapshot>) const override { return true; }
    bool Valid() const override { return true; }
    bool PublishIfValid(const std::function<bool()>& publish) const override {
        return publish();
    }
};

FallbackPacket MakePacket(std::uint64_t generation, std::uint64_t submission,
                          std::uint64_t packet_sequence) {
    struct Cs { std::array<std::byte, 32> bytes{}; } cs{};
    cs.bytes[0] = std::byte{0xA5};
    std::array<std::uint32_t, FallbackUserDataWords> user_data{};
    user_data[0] = 0x12345678;
    const SrtSnapshotProof proof{SrtCoverage::Complete, true, true, true, false};
    std::array<std::uint32_t, 2> code{0xBEEB03FF, 0x12345678};
    const auto code_bytes = std::as_bytes(std::span{code});
    std::vector<SrtReadSnapshot> reads{{0x3000, 7,
        std::vector<std::byte>{code_bytes.begin(), code_bytes.end()}}};
    return CaptureTypedFallbackPacket(
        GuestPacketIdentity{1, generation, submission, packet_sequence}, cs, user_data,
        std::span<const std::uint32_t>{}, reads, proof, std::nullopt, false,
        std::make_shared<TestLease>(),
        ShaderCodeSnapshot{0x3000, 0xA5,
                           std::vector<std::uint32_t>{code.begin(), code.end()}});
}

void SealedAscThenSafeGraphicsThenOrderedAsc() {
    CpCoordination<57> cp;
    FallbackMailbox mailbox;
    std::uint64_t generation{};
    assert(cp.Map(1, generation));
    CpCoordination<57>::SubmissionToken submission{};
    assert(cp.Issue(1, submission));

    const QidTick c_prefix{CpLane::Asc, 1, generation, 1};
    assert(cp.BeginRecord(CpLane::Asc, 1));
    auto c_seal = cp.SealAtLeast(c_prefix);
    assert(c_seal->State() == SealState::Pending);
    assert(!mailbox.Enqueue(MakePacket(generation, submission.sequence, 1), *c_seal));
    assert(!cp.PlanSeal(CpLane::Asc, false));
    assert(cp.EndRecord(CpLane::Asc, 1));
    auto c_plan = cp.PlanSeal(CpLane::Asc, false);
    assert(c_plan && cp.FinishSeal(*c_plan, true));
    assert(c_seal->State() == SealState::Done);

    auto request = mailbox.Enqueue(MakePacket(generation, submission.sequence, 1), *c_seal);
    assert(request);
    assert(!mailbox.TakeAtGraphicsSafePoint(false));
    assert(!request->GraphicsResult());
    auto taken = mailbox.TakeAtGraphicsSafePoint(true);
    assert(taken == request);
    assert(!mailbox.TakeAtGraphicsSafePoint(true));

    assert(cp.BeginRecord(CpLane::Graphics, 1));
    assert(!cp.PlanSeal(CpLane::Graphics, false));
    assert(cp.EndRecord(CpLane::Graphics, 1));
    assert(mailbox.MarkGraphicsRecorded(taken, generation, {1, 0, true, true, true}));
    const QidTick g_token{CpLane::Graphics, 0, cp.Generation(0), 1};
    auto g_seal = cp.SealAtLeast(g_token);
    assert(!mailbox.PublishGraphicsSeal(taken, *g_seal, generation));
    auto g_plan = cp.PlanSeal(CpLane::Graphics, false);
    assert(g_plan && cp.FinishSeal(*g_plan, true));
    assert(mailbox.PublishGraphicsSeal(taken, *g_seal, generation));
    assert(request->GraphicsResult() == g_token);

    // The next C packet can link only the G token that was actually sealed.
    assert(cp.AddDependency(CpTick{CpLane::Asc, cp.OpenTick(CpLane::Asc)},
                            *request->GraphicsResult()));
    assert(cp.BeginRecord(CpLane::Asc, 2));
    assert(cp.EndRecord(CpLane::Asc, 2));
    assert(cp.Complete(submission));
}

void RemapAndStopCancelUnpublishedFallback() {
    CpCoordination<57> cp;
    FallbackMailbox mailbox;
    std::uint64_t generation{};
    assert(cp.Map(1, generation));
    const QidTick c_prefix{CpLane::Asc, 1, generation, 1};
    auto c_seal = cp.SealAtLeast(c_prefix);
    auto c_plan = cp.PlanSeal(CpLane::Asc, false);
    assert(c_plan && cp.FinishSeal(*c_plan, true));
    auto request = mailbox.Enqueue(MakePacket(generation, 1, 1), *c_seal);
    assert(request);
    auto taken = mailbox.TakeAtGraphicsSafePoint(true);
    assert(taken == request);
    std::uint64_t closing{}, new_generation{};
    assert(cp.BeginClose(1, closing) && cp.FinishClose(1, closing));
    assert(cp.Map(1, new_generation) && new_generation != generation);
    assert(!mailbox.MarkGraphicsRecorded(taken, new_generation, {1, 0, true, true, true}));
    mailbox.Fail(taken);
    assert(request->State() == FallbackState::Failed && !request->GraphicsResult());

    auto queued = mailbox.Enqueue(MakePacket(generation, 1, 2), *c_seal);
    assert(queued);
    mailbox.Stop();
    assert(queued->State() == FallbackState::Cancelled && !queued->GraphicsResult());
}

} // namespace

int main() {
    SealedAscThenSafeGraphicsThenOrderedAsc();
    RemapAndStopCancelUnpublishedFallback();
}
