# SPDX-License-Identifier: GPL-2.0-or-later
"""Name the imports behind eboot call targets: python stub_names.py [--game DIR] 0xffa2a0 0x1cce0 ...

Each address is decoded as a PLT stub (jmp qword [rip+disp]); the GOT slot it reads is matched to
the eboot relocation that fills it. Reads only eboot.bin."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare import DEFAULT_GAME, module, names_database  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=DEFAULT_GAME)
    parser.add_argument('addresses', nargs='+', type=lambda s: int(s, 0))
    args = parser.parse_args()
    m = module(args.game / 'eboot.bin')
    hints, _ = names_database()
    loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = m['elf'][p['offset']:p['offset'] + p['filesz']]
    slots = {target: m['symbols'][sym] for target, kind, sym, _ in m['relocs'] if kind in (1, 6, 7)}
    for address in args.addresses:
        code = image[address:address + 6]
        if code[:2] != b'\xff\x25':
            print(f'{address:#x}: not a PLT stub ({code.hex()})')
            continue
        slot = address + 6 + struct.unpack('<i', code[2:6])[0]
        symbol = slots.get(slot)
        if not symbol:
            print(f'{address:#x}: GOT slot {slot:#x} has no import relocation')
            continue
        ident = symbol['identity']
        name = hints.get(ident['nid'], ident['nid']) if ident else symbol['name']
        library = ident['library']['name'] if ident else '?'
        print(f'{address:#x}: {name} ({library})')


if __name__ == '__main__':
    main()
