// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "g4_fallback_packet.h"
#include "srt_walker_capture.h"

using namespace iss::g4;

namespace {

struct TestLease final : SrtReadWatchLease {
    bool valid{true};
    bool Covers(std::span<const SrtReadSnapshot>) const override { return valid; }
    bool Valid() const override { return valid; }
    bool PublishIfValid(const std::function<bool()>& publish) const override {
        return valid && publish() && valid;
    }
};

FallbackPacket Capture(std::uint32_t qid, std::uint64_t generation,
                       bool indirect, SrtSnapshotProof proof) {
    struct Cs {
        std::array<std::byte, 12> words{};
    } cs{{std::byte{0x41}, std::byte{0x42}}};
    std::array<std::uint32_t, FallbackUserDataWords> ud{};
    ud[0] = 0x12345678;
    std::array<std::uint32_t, 4> flattened{17, 18, 19, 20};
    std::vector<SrtReadSnapshot> reads{{0x1000, 7,
                                        {std::byte{0x51}, std::byte{0x52},
                                         std::byte{0x00}, std::byte{0x00}}}};
    const auto args = indirect ? std::optional<IndirectArgsRef>{{0x2000, 9, 12}}
                               : std::nullopt;
    auto packet = CaptureTypedFallbackPacket({qid, generation, 1, 1}, cs, ud,
                                             flattened, reads, proof, args, indirect,
                                             std::make_shared<TestLease>(),
                                             ShaderCodeSnapshot{0x1000, 0x55, {0x00005251}});
    cs.words[0] = std::byte{0};
    ud[0] = 0;
    flattened[0] = 0;
    reads[0].bytes[0] = std::byte{0};
    assert(packet.compute_state[0] == std::byte{0x41});
    assert(packet.user_data[0] == 0x12345678);
    assert(packet.flattened_srt[0] == 17);
    assert(packet.srt_reads[0].bytes[0] == std::byte{0x51});
    return packet;
}

SrtSnapshotProof CompleteProof() {
    return {SrtCoverage::Complete, true, true, true};
}

void GateRequiresCompleteCoverageAndOwnerPartition() {
    auto packet = Capture(2, 3, true, {});
    assert(packet.Gate() == FallbackGate::IncompleteSrtCoverage);
    packet.srt_proof = CompleteProof();
    assert(packet.Gate() == FallbackGate::Ready);
    int publications{};
    assert(packet.PublishSrtIfValid([&] { ++publications; return true; }));
    assert(publications == 1);
    auto lease = packet.srt_lease;
    packet.srt_lease.reset();
    assert(packet.Gate() == FallbackGate::IncompleteSrtCoverage);
    assert(!packet.PublishSrtIfValid([&] { ++publications; return true; }));
    assert(publications == 1);
    packet.srt_lease = lease;
    assert(packet.indirect_args && packet.indirect_args->gpu_address == 0x2000);
    packet.indirect_args.reset();
    assert(packet.Gate() == FallbackGate::MissingIndirectReference);
    packet.indirect_args = IndirectArgsRef{0x2000, 9, 12};
    packet.required_compute_state_bytes += 4;
    assert(packet.Gate() == FallbackGate::MissingComputeState);
}

void PrefixSafePointAndReturnTokenPreserveOrder() {
    FallbackMailbox mailbox;
    auto first = Capture(2, 3, true, CompleteProof());
    auto second = Capture(2, 3, false, CompleteProof());
    second.guest.packet_sequence = 2;

    SealRequest c_prefix{{CpLane::Asc, 2, 3, 5}};
    assert(!mailbox.Enqueue(first, c_prefix)); // no ACK before C submitted
    c_prefix.Resolve(SealState::Done, 5);
    auto req1 = mailbox.Enqueue(first, c_prefix);
    auto req2 = mailbox.Enqueue(second, c_prefix);
    assert(req1 && req2);
    assert(!mailbox.TakeAtGraphicsSafePoint(false));
    auto recording1 = mailbox.TakeAtGraphicsSafePoint(true);
    assert(recording1 == req1);
    assert(!mailbox.TakeAtGraphicsSafePoint(true)); // cannot overtake first fallback
    assert(!mailbox.MarkGraphicsRecorded(req1, 4, {8, 4, true, true, true}));
    assert(!mailbox.MarkGraphicsRecorded(req1, 3, {8, 4, false, true, true}));
    assert(!mailbox.MarkGraphicsRecorded(req1, 3, {8, 3, true, true, true}));
    assert(mailbox.MarkGraphicsRecorded(req1, 3, {8, 4, true, true, true}));

    SealRequest g_seal{{CpLane::Graphics, 0, 1, 8}};
    assert(!mailbox.PublishGraphicsSeal(req1, g_seal, 3));
    assert(!req1->GraphicsResult());
    g_seal.Resolve(SealState::Done, 8);
    assert(mailbox.PublishGraphicsSeal(req1, g_seal, 3));
    assert(req1->GraphicsResult() == g_seal.target);
    assert(req1->Packet().guest.qid == 2 && req1->Packet().guest.generation == 3);
    assert(req1->GraphicsResult()->qid == 0); // G token is separate from guest identity

    auto recording2 = mailbox.TakeAtGraphicsSafePoint(true);
    assert(recording2 == req2);
    assert(mailbox.MarkGraphicsRecorded(req2, 3, {9, 4, true, true, true}));
    SealRequest g_seal2{{CpLane::Graphics, 0, 1, 9}};
    g_seal2.Resolve(SealState::Done, 9);
    assert(mailbox.PublishGraphicsSeal(req2, g_seal2, 3));
    assert(req2->GraphicsResult()->tick > req1->GraphicsResult()->tick);
}

void StopAndStaleGenerationDoNotReturnSuccess() {
    FallbackMailbox mailbox;
    SealRequest c_prefix{{CpLane::Asc, 1, 4, 1}};
    c_prefix.Resolve(SealState::Done, 1);
    auto packet = Capture(1, 4, true, CompleteProof());
    auto request = mailbox.Enqueue(packet, c_prefix);
    assert(request);
    assert(mailbox.TakeAtGraphicsSafePoint(true) == request);
    assert(!mailbox.MarkGraphicsRecorded(request, 5, {1, 4, true, true, true}));
    mailbox.Fail(request);
    assert(request->State() == FallbackState::Failed && !request->GraphicsResult());

    packet.guest.packet_sequence = 2;
    auto waiting = mailbox.Enqueue(packet, c_prefix);
    assert(waiting);
    mailbox.Stop();
    assert(waiting->State() == FallbackState::Cancelled);
    assert(!mailbox.TakeAtGraphicsSafePoint(true));
    assert(!mailbox.Enqueue(packet, c_prefix));
}

} // namespace

int main() {
    GateRequiresCompleteCoverageAndOwnerPartition();
    PrefixSafePointAndReturnTokenPreserveOrder();
    StopAndStaleGenerationDoNotReturnSuccess();
}
