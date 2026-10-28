"""Compile the actual shadow-key functions and check history/inheritance semantics."""
from pathlib import Path
import subprocess
import sys
from build_gpu import CLANG, ROOT, WORKSPACE, environment

out = Path(sys.argv[1]).resolve()
assert out.is_relative_to((ROOT / 'out/experimental').resolve())
out.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'gpu/native_shadow_rasterizer.inl').read_text()
functions = (source[source.index('static bool InactiveShadowUser('):source.index('static bool EquivalentStateEnabled(')]
             + source[source.index('static u64 BeginHash('):source.index('// The audited shape')])
test = r'''
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#define XXH_INLINE_ALL
#include "xxhash.h"
using u32 = std::uint32_t; using u64 = std::uint64_t;
namespace ns {
constexpr u32 VsUserData2=0x2C4E, DrawUserWords=6, MaxStateRegs=64;
struct Batch { std::vector<u32> state_regs; };
}
namespace AmdGpu { struct Regs { std::array<u32, 0x10000> reg_array{}; }; }
''' + functions + r'''
int main() {
    const ns::Batch batch{{0xA08F, 0x2C08, 0x2C48, 0xC242}};
    AmdGpu::Regs before{}, other{};
    const auto key = [&](const AmdGpu::Regs& regs, const std::array<u32, 4>& values) {
        return StateKey(batch, BeginHash(regs, batch), [&](u32 k) { return values[k]; });
    };
    const std::array<u32, 4> values{17, 23, 31, 43};
    const auto expected = key(before, values);
    for (u32 reg : {0x2C0Cu, 0x2C1Bu, 0x2C54u, 0x2C5Bu}) {
        other=before; other.reg_array[reg]=0x12345678;
        assert(key(other, values) == expected);
    }
    for (u32 reg : {0xA1F9u, 0xA1FAu, 0xA1FCu, 0xA29Eu, 0xA29Fu, 0xC24Cu, 0xC24Du}) {
        other=before; other.reg_array[reg]=0x12345678;
        assert(key(other, values) == expected);
    }
    // Different pre-range history must not split identical effective draw states.
    for (u32 reg : batch.state_regs) {
        other = before; other.reg_array[reg] = 0x12345678;
        assert(key(other, values) == expected);
    }
    // Inherited words are supplied by StateKey and still distinguish the state.
    for (u32 k=0; k<values.size(); ++k) {
        auto inherited=values; ++inherited[k];
        assert(key(before, inherited) != expected);
    }
    // Registers the batch does not cover must retain their influence.
    // This also models resubmission of the same batch/generation with a new
    // inherited shader setting: a persistent batch-only hash cache is invalid.
    for (u32 reg : {0xA090u, 0x2C09u, 0x2C49u, 0x2C4Au, 0xC243u}) {
        other=before; other.reg_array[reg]=7;
        assert(key(other, values) != expected);
    }
    std::puts("PASS: shader/context/uconfig history, inherited values, untouched state");
}
'''
cpp = out / 'shadow_key_test.cpp'
exe = out / 'shadow_key_test.exe'
cpp.write_text(test)
subprocess.run([str(CLANG), '/nologo', '/std:c++20', '/EHsc', '/MD',
                '/I' + str(WORKSPACE / 'research/shadPS4-perf/externals/xxhash'),
                str(cpp), '/Fe:' + str(exe), '/Fo:' + str(out / 'shadow_key_test.obj')],
               env=environment(), check=True, cwd=out)
subprocess.run([str(exe)], check=True)
