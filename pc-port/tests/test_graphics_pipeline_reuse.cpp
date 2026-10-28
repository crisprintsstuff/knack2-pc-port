// SPDX-License-Identifier: GPL-2.0-or-later
#include "graphics_pipeline_reuse.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using iss::pipeline_reuse::StageIdentity;
using iss::pipeline_reuse::StageMatches;
using iss::pipeline_reuse::Stamp;

static void Require(bool value, const char* what) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

int main() {
    int programs[2]{};
    const StageIdentity saved{programs, 0x100, 0x12345678, 3, 96};
    std::array<std::byte, 16> registers{}, runtime{}, user_data{};
    registers[0] = std::byte{0x11};
    runtime[0] = std::byte{0x22};
    user_data[0] = std::byte{0x33};
    const auto accepts = [&](const StageIdentity& current, auto header, auto state, auto scout_ud) {
        return StageMatches(saved, current, registers, header, runtime, state, user_data, scout_ud);
    };
    Require(accepts(saved, registers, runtime, user_data), "identical resolved proof accepted");
    auto identity = saved;
    identity.program = programs + 1;
    Require(!accepts(identity, registers, runtime, user_data), "same shader hash, another Program");
    identity = saved; identity.module++;
    Require(!accepts(identity, registers, runtime, user_data), "module replacement cannot reuse");
    identity = saved; identity.permutation++;
    Require(!accepts(identity, registers, runtime, user_data), "changed specialization cannot reuse");
    identity = saved; identity.shader_hash++;
    Require(!accepts(identity, registers, runtime, user_data), "changed binary identity");
    identity = saved; identity.code_dwords++;
    Require(!accepts(identity, registers, runtime, user_data), "changed binary size");
    auto changed = registers; changed.back() ^= std::byte{1};
    Require(!accepts(saved, changed, runtime, user_data), "SH header changed outside shader address");
    changed = runtime; changed.back() ^= std::byte{1};
    Require(!accepts(saved, registers, changed, user_data), "runtime change despite same permutation");
    changed = user_data; changed.back() ^= std::byte{1};
    Require(!accepts(saved, registers, runtime, changed), "stale scout user data rejected");
    user_data = changed;
    Require(accepts(saved, registers, runtime, changed), "fresh resource addresses allowed");
    Require(!StageMatches(saved, saved, registers, std::span(registers).first(8),
                          runtime, runtime, user_data, user_data), "incomplete header rejected");
    Require(!StageMatches(saved, saved, registers, registers, runtime,
                          std::span(runtime).first(8), user_data, user_data), "incomplete runtime rejected");
    const Stamp stamp{1, 2, 3, 4, 5, 6, 7};
    for (int field = 0; field < 7; ++field) {
        auto next = stamp;
        switch (field) {
        case 0: ++next.context; break;
        case 1: ++next.upload_epoch; break;
        case 2: ++next.host_write; break;
        case 3: ++next.recording_tick; break;
        case 4: ++next.stages; break;
        case 5: ++next.vertex_offset; break;
        case 6: ++next.instance_offset; break;
        }
        Require(next != stamp, "every state/ordering boundary invalidates");
    }
    std::puts("graphics pipeline reuse proof checks passed");
}
