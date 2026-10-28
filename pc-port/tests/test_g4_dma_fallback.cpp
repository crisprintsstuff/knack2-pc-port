// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <memory>

#include "g4_asc_owner_pause.h"
#include "g4_cp_coordination.h"
#include "g4_dma_fallback.h"

using namespace iss::g4;

int main() {
    DmaFallbackPacket packet{2, 4, 7, 3, DmaFallbackKind::Copy,
                             0x2000, 0x1000, 64, 0, false, false, false};
    assert(packet.Valid());
    const DmaFallbackPacket captured = packet;
    packet.src = 0; // guest PM4 storage may change after publication
    packet.dst = 0;

    GraphicsControlMailbox mailbox;
    AscOwnerPause pause;
    auto result = std::make_shared<DmaFallbackResult>();
    const auto request = mailbox.Enqueue([captured, result] {
        assert(captured.src == 0x1000 && captured.dst == 0x2000);
        result->graphics_tick = 9;
        result->status = DmaFallbackStatus::Ready;
    });
    assert(pause.Begin(2, request, 5));
    assert(pause.Next().gate == AscOwnerPause::Gate::Waiting);
    assert(!pause.AcknowledgePaused(5, 4, true));
    assert(pause.AcknowledgePaused(5, 5, true));
    assert(mailbox.ServeOne(true));
    assert(request->Status() == GraphicsControlMailbox::Control::State::Done);
    assert(pause.Next().gate == AscOwnerPause::Gate::ResumeOrigin);
    assert(!pause.AcknowledgeOriginResume(3)); // no other ASC qid may overtake
    assert(pause.AcknowledgeOriginResume(2));
    assert(result->status == DmaFallbackStatus::Ready && result->graphics_tick == 9);

    CpCoordination<57> cp;
    std::uint64_t asc_generation{};
    assert(cp.Map(2, asc_generation));
    const QidTick graphics{CpLane::Graphics, 0, cp.Generation(0), result->graphics_tick};
    // The next C packet depends on the actual G tick; the prior C prefix is
    // already sealed before the paused graphics control starts.
    for (std::uint64_t tick = 1; tick <= graphics.tick; ++tick)
        assert(cp.PublishSubmitted(CpLane::Graphics, tick));
    assert(cp.AddDependency({CpLane::Asc, cp.OpenTick(CpLane::Asc)}, graphics));
    assert(cp.ActiveDependencies() == 0);

    DmaFallbackPacket fill{1, 1, 1, 1, DmaFallbackKind::Fill,
                           0x4000, 0, 16, 0xA5A5A5A5, false, false, true};
    assert(fill.Valid());
    fill.src_gds = true;
    assert(!fill.Valid());
}
