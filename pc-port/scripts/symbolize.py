# SPDX-License-Identifier: GPL-2.0-or-later
"""Names the iss-gpu.dll offsets in a hang report or log from the DLL's link map.

python scripts/symbolize.py [report]   (default: out/hang-report.txt; map: out/iss-gpu.map)

Each "iss-gpu.dll+0x..." gains "(function+0xoffset)". MSVC-mangled names are demangled with
tools/llvm/bin/llvm-undname.exe when present.
"""
import bisect
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UNDNAME = ROOT.parent / 'tools/llvm/bin/llvm-undname.exe'


def load_map(path):
    base, symbols = 0x180000000, []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
        if match:
            base = int(match.group(1), 16)
            continue
        # lld-link: "Address Size Align Out In Symbol"; symbol rows are the deepest indented.
        fields = line.split()
        if len(fields) == 4 and re.fullmatch(r'[0-9a-fA-F]{8,16}', fields[0]) and ':(' not in fields[3] and not fields[3].startswith('.'):
            symbols.append((int(fields[0], 16), fields[3]))
    symbols.sort()
    return base, symbols


def demangle(names):
    result = {n: n for n in names}
    if UNDNAME.exists():
        for n in names:
            if n.startswith('?'):
                out = subprocess.run([str(UNDNAME), n], capture_output=True, text=True).stdout
                lines = [l.strip() for l in out.splitlines() if l.strip()]
                if lines:
                    result[n] = lines[-1]
    return result


def main():
    report = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'out/hang-report.txt'
    base, symbols = load_map(ROOT / 'out/iss-gpu.map')
    addresses = [a for a, _ in symbols]
    text = report.read_text(encoding='utf-8', errors='replace')
    found = {}
    for offset in {int(m, 16) for m in re.findall(r'iss-gpu\.dll\+0x([0-9a-fA-F]+)', text)}:
        i = bisect.bisect_right(addresses, base + offset) - 1
        if i >= 0:
            found[offset] = (symbols[i][1], base + offset - symbols[i][0])
    names = demangle({name for name, _ in found.values()})
    def name(match):
        offset = int(match.group(1), 16)
        if offset not in found:
            return match.group(0)
        symbol, delta = found[offset]
        return f'{match.group(0)}({names.get(symbol, symbol)}+0x{delta:x})'
    print(re.sub(r'iss-gpu\.dll\+0x([0-9a-fA-F]+)', name, text))


if __name__ == '__main__':
    main()
