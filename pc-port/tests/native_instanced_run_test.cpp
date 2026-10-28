// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/infamous_renderer/native_instanced_run.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace iss::native_geometry;
using Words = std::vector<std::uint32_t>;

static void Packet(Words& words, unsigned opcode,
                   std::initializer_list<std::uint32_t> payload) {
    words.push_back(0xc0000000u | (unsigned(payload.size() - 1) << 16) | (opcode << 8));
    words.insert(words.end(), payload);
}

int main() {
    std::array<std::uint32_t, 128> regs{};
    regs[42] = 0x12340000; regs[43] = 2;
    regs[44] = 0x23000000; regs[45] = 0x480000; regs[46] = 1;
    regs[47] = 0x20000000; regs[92] = 4;
    InstancedRunInputs inputs{regs, 32, 90, 42, 44, 0,
        {0x212340000ull, {regs[44], regs[45], regs[46], regs[47]},
         0x250000000ull, 3, 3, 0, 1, 0}};

    Words prefix;
    Packet(prefix, 0x10, {0, 0xdeadbeef});
    Packet(prefix, 0x79, {2, 4});
    Packet(prefix, 0x2a, {0});
    Packet(prefix, 0x2f, {3});
    Packet(prefix, 0x76, {10, 0x12340030, 2, 0x23000090, 0x480000, 3,
                           0x20000000});
    const auto second_draw_word = prefix.size();
    Packet(prefix, 0x27, {6, 0x60000000, 2, 6, 0});
    const auto consumed = prefix.size();
    Packet(prefix, 0x76, {12, 0x23000120}); // Leave a trailing setter for normal CP.
    const auto run = CollectInstancedRun(prefix, inputs);
    assert(run.draws.size() == 2 && run.consumed_words == consumed &&
           run.packet_count == 6);
    assert(run.draws[1].fetch_descriptors == 0x212340030ull);
    assert(run.draws[1].ssbo_raw[0] == 0x23000090 && run.draws[1].ssbo_raw[2] == 3);
    assert(run.draws[1].instances == 3 && run.draws[1].index_address == 0x260000000ull);
    assert(run.draws[1].packet_word == second_draw_word && regs[44] == 0x23000000);
    assert(CollectInstancedRun(prefix, inputs, 64, 5).draws.empty());
    assert(CollectInstancedRun(prefix, inputs, 64, 6).consumed_words == consumed);

    unsigned checks = 0;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        if (opcode == 0x10 || opcode == 0x76 || opcode == 0x79 ||
            opcode == 0x2a || opcode == 0x2f || opcode == 0x27) continue;
        Words words;
        Packet(words, opcode, {0});
        words.insert(words.end(), prefix.begin(), prefix.end());
        assert(CollectInstancedRun(words, inputs).draws.empty());
        ++checks;
    }
    for (std::size_t n = 0; n < consumed; ++n) {
        assert(CollectInstancedRun(std::span(prefix).first(n), inputs).draws.empty());
        ++checks;
    }
    for (auto changed : {Words{0xc0017600, 8, 1},
                         Words{0xc0017900, 2, 5},
                         Words{0xc0002a00, 1},
                         Words{0xc0002f00, 0},
                         Words{0xc0002f00, 257},
                         Words{0xc0001000, 0x68750001},
                         Words{0xc0017600, 200, 0}}) {
        changed.insert(changed.end(), prefix.begin(), prefix.end());
        assert(CollectInstancedRun(changed, inputs).draws.empty());
        ++checks;
    }
    Words many;
    for (unsigned i = 0; i < 70; ++i) {
        Packet(many, 0x2f, {1 + i % 16});
        Packet(many, 0x27, {3, 0x70000000 + i * 2, 2, 3, 0});
    }
    const auto bounded = CollectInstancedRun(many, inputs, 64);
    assert(bounded.draws.size() == 64 && bounded.packet_count == 126);
    assert(CollectInstancedRun(many, inputs, 257).draws.empty());
    std::printf("PASS: %u boundaries/truncations; bounded instances and draw order.\n", checks);
}
