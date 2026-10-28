// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>

#include "g4_packet_hazards.h"

using namespace iss::g4;

int main() {
    PacketHazards hazards;
    constexpr PacketResourceAccess a_write{10, 0, 4096, true};
    constexpr PacketResourceAccess a_read{10, 512, 128, false};
    constexpr PacketResourceAccess b_read{20, 0, 4096, false};
    const PacketLeaseKey gfx{CpLane::Graphics, 0, 1, 1};
    const PacketLeaseKey asc{CpLane::Asc, 2, 7, 1};

    const auto first = hazards.TryReserve(gfx, std::array{a_write}, 3, 3);
    assert(first.status == PacketReserveStatus::Reserved);
    constexpr PacketResourceAccess global_graphics{GlobalResourceKey, 0, 1, true};
    const auto global_busy = hazards.TryReserve(
        {CpLane::Graphics, 0, 1, 2}, std::array{global_graphics}, 3, 3);
    assert(global_busy.status == PacketReserveStatus::Busy);
    // The free B access must not be kept when the second access conflicts with A.
    const auto mixed = hazards.TryReserve(asc, std::array{b_read, a_read}, 3, 3);
    assert(mixed.status == PacketReserveStatus::Busy && mixed.lease_id == 0);
    const auto b_only = hazards.TryReserve(asc, std::array{b_read}, 3, 3);
    assert(b_only.status == PacketReserveStatus::Reserved);
    assert(hazards.CancelUnpublished(b_only.lease_id));
    assert(hazards.Revalidate(first.lease_id, 3, true));
    assert(!hazards.Revalidate(first.lease_id, 4, true));
    assert(hazards.Publish(first.lease_id, {CpLane::Graphics, 0, 1, 5}, 3, true));
    assert(!hazards.CancelUnpublished(first.lease_id));
    assert(hazards.FinishRecord(first.lease_id));

    const auto global = hazards.TryReserve(
        {CpLane::Graphics, 0, 1, 2}, std::array{global_graphics}, 3, 3);
    assert(global.status == PacketReserveStatus::Reserved);
    hazards.InvalidateCpuWrite(GuestBackingKey, 1024, 4);
    assert(hazards.Revalidate(global.lease_id, 3, true));
    assert(hazards.TryReserve(asc, std::array{b_read}, 3, 3).status ==
           PacketReserveStatus::Busy);
    assert(hazards.CancelUnpublished(global.lease_id));

    const auto later = hazards.TryReserve(asc, std::array{a_read}, 3, 3);
    assert(later.status == PacketReserveStatus::Reserved);
    const auto dependencies = hazards.Dependencies(later.lease_id);
    assert(dependencies.size() == 1 && dependencies[0] ==
           (QidTick{CpLane::Graphics, 0, 1, 5}));
    hazards.InvalidateCpuWrite(10, 520, 8);
    assert(!hazards.Revalidate(later.lease_id, 3, true));
    assert(hazards.CancelUnpublished(later.lease_id));
    hazards.RetireCompleted(CpLane::Graphics, 5);

    // G records through tick 6, flushes inside Draw, then records into tick 7.
    // Its global lease stays unpublished until the final tick is known. C cannot
    // reserve during either tick, and afterwards depends on 7 rather than 6.
    const auto spanning = hazards.TryReserve(
        {CpLane::Graphics, 0, 1, 3}, std::array{global_graphics}, 3, 3);
    assert(spanning.status == PacketReserveStatus::Reserved);
    assert(hazards.TryReserve(asc, std::array{a_read}, 3, 3).status ==
           PacketReserveStatus::Busy);
    hazards.InvalidateCpuWrite(GuestBackingKey, 512, 4);
    assert(hazards.Revalidate(spanning.lease_id, 3, true));
    assert(hazards.Publish(spanning.lease_id, {CpLane::Graphics, 0, 1, 7}, 3, true));
    assert(hazards.FinishRecord(spanning.lease_id));
    const auto after_span = hazards.TryReserve(asc, std::array{a_read}, 3, 3);
    assert(after_span.status == PacketReserveStatus::Reserved);
    assert(hazards.Dependencies(after_span.lease_id) ==
           (std::vector<QidTick>{{CpLane::Graphics, 0, 1, 7}}));
    assert(hazards.CancelUnpublished(after_span.lease_id));
    hazards.RetireCompleted(CpLane::Graphics, 7);

    const auto stale = hazards.TryReserve(asc, std::array{a_read}, 3, 4);
    assert(stale.status == PacketReserveStatus::Stale && stale.lease_id == 0);
    const auto fresh = hazards.TryReserve(asc, std::array{a_read}, 4, 4);
    assert(fresh.status == PacketReserveStatus::Reserved);
    assert(hazards.Dependencies(fresh.lease_id).empty());
    hazards.InvalidateUnpublished();
    assert(!hazards.Revalidate(fresh.lease_id, 4, true));
    assert(hazards.CancelUnpublished(fresh.lease_id));
    const auto remapped = hazards.TryReserve(asc, std::array{a_read}, 5, 5);
    assert(remapped.status == PacketReserveStatus::Reserved);
    hazards.Stop();
    assert(!hazards.Revalidate(remapped.lease_id, 5, true));
    assert(hazards.TryReserve(gfx, std::array{a_write}, 4, 4).status ==
           PacketReserveStatus::Stopped);

    PacketHazards fifo;
    const PacketLeaseKey c1{CpLane::Asc, 1, 1, 1};
    const PacketLeaseKey c2{CpLane::Asc, 2, 1, 1};
    auto p1 = fifo.TryReserve(c1, std::array{a_write}, 1, 1);
    assert(p1.status == PacketReserveStatus::Reserved);
    assert(fifo.Publish(p1.lease_id, {CpLane::Asc, 1, 1, 2}, 1, true));
    assert(fifo.FinishRecord(p1.lease_id));
    auto p2 = fifo.TryReserve(c2, std::array{b_read}, 1, 1);
    assert(p2.status == PacketReserveStatus::Reserved);
    assert(fifo.Publish(p2.lease_id, {CpLane::Asc, 2, 1, 3}, 1, true));
    assert(fifo.FinishRecord(p2.lease_id));
    auto pg = fifo.TryReserve(gfx, std::array{global_graphics}, 1, 1);
    assert(pg.status == PacketReserveStatus::Reserved);
    const auto newest = fifo.Dependencies(pg.lease_id);
    assert(newest.size() == 1 && newest[0] == (QidTick{CpLane::Asc, 2, 1, 3}));
    assert(fifo.CancelUnpublished(pg.lease_id));
    fifo.RetireCompleted(CpLane::Asc, 3);
    auto after_done = fifo.TryReserve(gfx, std::array{global_graphics}, 1, 1);
    assert(after_done.status == PacketReserveStatus::Reserved);
    assert(fifo.Dependencies(after_done.lease_id).empty());
    assert(fifo.CancelUnpublished(after_done.lease_id));
}
