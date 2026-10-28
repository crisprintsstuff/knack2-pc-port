"""Index of the game's compute shaders for the ahead-of-time compiler (Claude, 2026-10-19).

python scripts/aot_index.py <game_dir> <out_index>

Reads the player's own game files: every .xpps pack holds the PS4 shader binaries (code followed
by the OrbShdr footer) and, for compute shaders, the Gnmx CsShader structure with the registers
the game sets at dispatch (COMPUTE_PGM_RSRC1/2, COMPUTE_NUM_THREAD_X/Y/Z). Pack layout: 'PACK'
header, u32 at 0x28 = base of the data section; the structure's program address is an offset from
that base, the code starts with s_mov_b32 vcc_hi, N and its footer is (N + 1) * 8 bytes after.

Index (little endian): 'ISSAOT01', u32 count, then per shader: u64 hash, u32 rsrc1, u32 rsrc2,
u32 num_thread x, y, z (0 for pixel and vertex shaders), u32 code_dwords, u32 type
(OrbShdr: 0 pixel, 1 vertex, 4 compute), u16 path length, path (UTF-8, relative to game_dir),
u64 code offset. One entry per hash (the first pack holding it); conflicting registers for one
hash are reported."""
import mmap
import os
import struct
import sys
import time

import numpy as np

MARK = 0xBEEB03FF


def scan(path):
    out = []
    with open(path, 'rb') as fh:
        size = os.path.getsize(path)
        if size < 0x100:
            return out, 0
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            if mm[:4] != b'KCAP':
                return out, 0
            base, = struct.unpack_from('<I', mm, 0x28)
            starts = {}
            i = mm.find(b'OrbShdr')
            while i >= 0:
                if i + 32 <= size:
                    word, = struct.unpack_from('<I', mm, i + 8)
                    stype = (word >> 2) & 0xF
                    if stype in (0, 1, 4):
                        length = word >> 8
                        h, = struct.unpack_from('<Q', mm, i + 16)
                        # The code start: the vcc_hi token whose count lands on this footer.
                        lo = max(0, i - length - 0x4000) & ~3
                        j = mm.rfind(struct.pack('<I', MARK), lo, i)
                        while j >= lo:
                            n, = struct.unpack_from('<I', mm, j + 4)
                            if j + (n + 1) * 8 == i:
                                starts[j] = (h, length, stype)
                                break
                            j = mm.rfind(struct.pack('<I', MARK), lo, j)
                i = mm.find(b'OrbShdr', i + 7)
            if not starts:
                return out, 0
            # CsShader: u64 program offset (from base), rsrc1, rsrc2, threads x, y, z.
            a = np.frombuffer(mm, dtype='<u4', count=size // 4)
            want = np.array(sorted(s - base for s, v in starts.items() if s >= base and v[2] == 4),
                            dtype=np.uint32)
            hit = np.nonzero(np.isin(a[:-7], want) & (a[1:-6] == 0))[0]
            found = set()
            for k in hit:
                pgm = int(a[k]); r1, r2, x, y, z = (int(v) for v in a[k + 2:k + 7])
                if not (0 < x <= 1024 and 0 < y <= 1024 and 0 < z <= 64 and x * y * z <= 1024):
                    continue
                if (r2 >> 1) & 0x1F > 16:
                    continue
                s = pgm + base
                h, length, _ = starts[s]
                out.append((h, r1, r2, x, y, z, length // 4, s, 4))
                found.add(s)
            del a, hit
            # Pixel (0) and vertex (1) shaders: their code only (the registers come with the
            # pipeline recipes).
            for s, (h, length, stype) in starts.items():
                if stype != 4:
                    out.append((h, 0, 0, 0, 0, 0, length // 4, s, stype))
            return out, sum(1 for s, v in starts.items() if v[2] == 4 and s not in found)
        finally:
            mm.close()


def main():
    root, dest = sys.argv[1], sys.argv[2]
    files = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(root) for f in fs if f.endswith('.xpps'))
    t0 = time.time()
    entries = {}
    conflicts = 0
    missing_regs = 0
    for n, path in enumerate(files):
        rows, missing = scan(path)
        missing_regs += missing
        rel = os.path.relpath(path, root)
        for h, r1, r2, x, y, z, dwords, off, stype in rows:
            e = entries.get(h)
            if e is None:
                entries[h] = (r1, r2, x, y, z, dwords, rel, off, stype)
            elif e[:6] != (r1, r2, x, y, z, dwords):
                conflicts += 1
        if n % 100 == 0:
            print(f'{n}/{len(files)} packs, {len(entries)} compute shaders, {time.time() - t0:.0f} s', flush=True)
    with open(dest, 'wb') as f:
        f.write(b'ISSAOT01')
        f.write(struct.pack('<I', len(entries)))
        for h, (r1, r2, x, y, z, dwords, rel, off, stype) in sorted(entries.items()):
            p = rel.encode('utf-8')
            f.write(struct.pack('<Q7IH', h, r1, r2, x, y, z, dwords, stype, len(p)))
            f.write(p)
            f.write(struct.pack('<Q', off))
    print(f'shaders {len(entries)} (compute {sum(1 for e in entries.values() if e[8] == 4)}), register conflicts {conflicts}, '
          f'code without registers (occurrences) {missing_regs}, {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
