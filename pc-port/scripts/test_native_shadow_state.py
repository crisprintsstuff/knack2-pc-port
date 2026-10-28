"""Check actual state equivalence helper, including inherited/active state guards."""
from pathlib import Path
import subprocess
import sys
from build_gpu import CLANG, ROOT, environment

out = Path(sys.argv[1]).resolve()
assert out.is_relative_to((ROOT / 'out/experimental').resolve())
out.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'gpu/native_shadow_rasterizer.inl').read_text()
helpers = (source[source.index('static bool InactiveShadowUser('):source.index('static bool EquivalentStateEnabled(')]
           + source[source.index('static bool EquivalentState('):source.index('static bool ResourceReuseEnabled(')])
test = '#include "' + (ROOT / 'gpu/native_shadow.h').as_posix() + '"\n'
test += '#include <cassert>\n#include <cstdio>\nusing u32=std::uint32_t; namespace ns=iss::native_shadow;\n'
test += helpers + r'''
int main() {
    ns::Batch b;
    b.state_regs = {0x2C54, 0xA003, 0xA191};
    b.states = {{4, 0}, {0, 3}};
    b.state_values = {99, 7, 0, 77, 7, 9};
    u32 entry[] = {0, 7, 9};
    assert(EquivalentState(b, 0, 1, entry));
    entry[2] = 10;
    assert(!EquivalentState(b, 0, 1, entry));
    entry[2] = 9;
    b.state_values[4] = 8;
    assert(!EquivalentState(b, 0, 1, entry));
    b.state_values[4] = 7;
    b.state_values[3] = 0xDEADBEEF;
    assert(EquivalentState(b, 0, 1, entry));
    assert(!EquivalentState(b, 0, ~0u, entry));
    std::puts("PASS: effective state equivalence preserves inherited/active values");
}
'''
cpp = out / 'shadow_state_test.cpp'
exe = out / 'shadow_state_test.exe'
cpp.write_text(test)
subprocess.run([str(CLANG), '/nologo', '/std:c++20', '/EHsc', '/MD', str(cpp),
                '/Fe:' + str(exe), '/Fo:' + str(out / 'shadow_state_test.obj')],
               env=environment(), check=True, cwd=out)
subprocess.run([str(exe)], check=True)
