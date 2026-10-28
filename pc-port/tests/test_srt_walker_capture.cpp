// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <set>

#include "srt_walker_capture.h"

using namespace iss::g4;

namespace {

struct Memory final : SrtMemoryProvider {
    std::map<std::uint64_t, std::byte> bytes;
    std::uint64_t epoch{1};
    std::uint64_t mapping{1};
    bool epoch_available{true};
    bool mutate_during_copy{};
    bool watch_enabled{};
    bool redirect_on_arm{};

    struct Watch final : SrtReadWatchLease {
        const Memory& memory;
        std::uint64_t epoch;
        std::uint64_t mapping;
        std::set<std::pair<std::uint64_t, std::size_t>> ranges;
        Watch(const Memory& memory_, std::uint64_t epoch_, std::uint64_t mapping_)
            : memory{memory_}, epoch{epoch_}, mapping{mapping_} {}
        bool Covers(std::span<const SrtReadSnapshot> reads) const override {
            for (const auto& read : reads) {
                if (!ranges.contains({read.guest_address, read.bytes.size()})) return false;
            }
            return true;
        }
        bool Valid() const override {
            return memory.epoch == epoch && memory.mapping == mapping;
        }
        bool PublishIfValid(const std::function<bool()>& publish) const override {
            return Valid() && publish() && Valid();
        }
    };

    void Put32(std::uint64_t address, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[address + i] = std::byte(value >> (8 * i));
        ++epoch;
    }
    void Put64(std::uint64_t address, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes[address + i] = std::byte(value >> (8 * i));
        ++epoch;
    }
    std::optional<std::uint64_t> GlobalEpoch() override {
        return epoch_available ? std::optional{epoch} : std::nullopt;
    }
    std::shared_ptr<SrtReadWatchLease> ArmWatchSet(
        std::span<const SrtReadSnapshot> reads) override {
        if (!watch_enabled) return {};
        if (redirect_on_arm) {
            redirect_on_arm = false;
            Put64(0x1000, 0x3000);
            Put32(0x3004, 0xeeeeffff);
        }
        auto watch = std::make_shared<Watch>(*this, epoch, mapping);
        for (const auto& read : reads) watch->ranges.insert({read.guest_address,
                                                              read.bytes.size()});
        return watch;
    }
    bool Version(std::uint64_t address, std::size_t size,
                 SrtMemoryVersion& out) override {
        for (std::size_t i = 0; i < size; ++i) if (!bytes.contains(address + i)) return false;
        out.mapping_generation = mapping;
        out.cpu_versions = {epoch};
        return true;
    }
    bool CopySparse(std::uint64_t address, std::span<std::byte> out) override {
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            out[i] = it->second;
        }
        if (mutate_during_copy) { ++epoch; mutate_during_copy = false; }
        return true;
    }
};

// Mirrors a generated walker: a root user-data pointer, a transitive pointer,
// and two dword loads. The actual generator is compiled separately.
void PS4_SYSV_ABI NestedWalker(const std::uint32_t* ud, std::uint32_t* flat) {
    const auto root = std::uint64_t{ud[0]} | (std::uint64_t{ud[1]} << 32);
    const auto child = ReadCapturedSrt(root, 8) & 0xFFFFFFFFFFFFULL;
    flat[16] = static_cast<std::uint32_t>(ReadCapturedSrt(root + 8, 4));
    flat[17] = static_cast<std::uint32_t>(ReadCapturedSrt(child + 4, 4));
}

} // namespace

int main() {
    Memory mem;
    mem.Put64(0x1000, 0x2000);
    mem.Put32(0x1008, 0xaaaabbbb);
    mem.Put32(0x2004, 0xccccdddd);
    Shader::PersistentSrtInfo info{};
    info.walker_func = NestedWalker;
    info.capture_walker_func = NestedWalker;
    info.capture_walker_func_size = 1;
    info.capture_schema = SrtCaptureSchema;
    info.capture_relocations = {2};
    info.flattened_bufsize_dw = 18;
    std::array<std::uint32_t, 16> ud{};
    ud[0] = 0x1000;

    auto captured = CaptureSrtSnapshot(info, ud, mem);
    assert(captured.proof.coverage == SrtCoverage::Complete);
    assert(captured.reads.size() == 3);
    assert(captured.reads[0].guest_address == 0x1000 && captured.reads[0].bytes.size() == 8);
    assert(captured.reads[1].guest_address == 0x1008);
    assert(captured.reads[2].guest_address == 0x2004);
    assert(captured.flattened[16] == 0xaaaabbbb);
    assert(captured.flattened[17] == 0xccccdddd);
    ud[0] = 0;
    mem.Put32(0x2004, 0);
    assert(captured.flattened[17] == 0xccccdddd); // owning bytes
    assert(!RevalidateSrtSnapshot(captured, mem));

    ud[0] = 0x1000;
    mem.mutate_during_copy = true;
    assert(CaptureSrtSnapshot(info, ud, mem).proof.coverage == SrtCoverage::Unknown);
    mem.epoch_available = false;
    assert(CaptureSrtSnapshot(info, ud, mem).proof.coverage == SrtCoverage::Unknown);
    mem.watch_enabled = true;
    mem.redirect_on_arm = true;
    auto watched = CaptureSrtSnapshot(info, ud, mem);
    assert(watched.proof.coverage == SrtCoverage::Complete);
    assert(watched.capture_epoch == 0 && watched.watch_lease);
    assert(watched.flattened[17] == 0xeeeeffff);
    assert(watched.reads[2].guest_address == 0x3004);
    assert(RevalidateSrtSnapshot(watched, mem));
    mem.Put32(0x3004, 0);
    assert(!RevalidateSrtSnapshot(watched, mem));
    mem.Put32(0x4000, 0x11223344);
    const std::array extra{SrtExtraRead{0x4000, 4}};
    auto with_code = CaptureSrtSnapshot(info, ud, mem, extra);
    assert(with_code.proof.coverage == SrtCoverage::Complete);
    assert(with_code.reads.back().guest_address == 0x4000);
    assert(with_code.reads.back().bytes.size() == 4);
    mem.Put32(0x4000, 0);
    assert(!RevalidateSrtSnapshot(with_code, mem));
    mem.watch_enabled = false;
    mem.epoch_available = true;
    ++mem.mapping;
    auto remapped = CaptureSrtSnapshot(info, ud, mem);
    assert(remapped.proof.coverage == SrtCoverage::Complete);
    ++mem.mapping;
    assert(!RevalidateSrtSnapshot(remapped, mem));

    info.capture_walker_func = nullptr;
    assert(CaptureSrtSnapshot(info, ud, mem).proof.coverage == SrtCoverage::Unknown);
}
