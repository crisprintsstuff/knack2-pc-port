"""Merge the trusted pipeline pack with several independent cache lineages.

Permutation indexes are local to each lineage: program P may have permutation 1
meaning different specializations in two caches. Instead of rejecting those
conflicts, every permutation is identified by its full content (metadata body +
SPIR-V) and given a fresh index inside the merged pack; the pipeline keys that
reference it are rewritten with the new HashCombine(program, index).

The base pack keeps its own indexes and bytes. Sources are read-only. Lineages
whose profile.bin differs, metadata with another binary version, and pipeline
keys with another serialized size are skipped (counted in selection.json).
No driver binaries are copied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MASK = (1 << 64) - 1
SPIRV_MAGIC = 0x07230203
META_VERSIONS = (6, 7)
BINARY_VERSION = 8
KEY_VERSION = 3


def sha(data):
    return hashlib.sha256(data).hexdigest()


def hash_combine(shader, permutation):
    return (shader ^ (permutation + 0x9e3779b9 + ((shader << 12) & MASK) + (shader >> 4))) & MASK


def read_pack(path):
    m = json.loads((path / 'manifest.json').read_text(encoding='utf-8'))
    packed = (path / 'recipes.bin').read_bytes()
    assert sha(packed) == m['pack']['sha256'], 'pack hash mismatch'
    files = {}
    for item in m['assets']:
        data = packed[item['offset']:item['offset'] + item['size']]
        assert len(data) == item['size'] and sha(data) == item['sha256'], item['filename']
        files[item['filename']] = data
    profile = (path / 'profile.bin').read_bytes()
    assert sha(profile) == m['profile']['sha256']
    return files, profile


def read_dir(path):
    files = {}
    for f in path.iterdir():
        if f.suffix in ('.key', '.meta', '.spv') and f.is_file() and not f.is_symlink():
            files[f.name] = f.read_bytes()
    return files


class Merged:
    def __init__(self, graphics_key_size):
        self.graphics_key_size = graphics_key_size
        self.perms = {}       # program -> list of (meta_version, meta_body, spv)
        self.content = {}     # (program, sha(meta_body), sha(spv)) -> index
        self.keys = {}        # serialized key bytes -> kind
        self.stats = {}

    def count(self, source, what, n=1):
        self.stats.setdefault(source, {}).setdefault(what, 0)
        self.stats[source][what] += n

    def add_lineage(self, source, files, keep_indexes):
        """Adds one lineage; returns nothing. keep_indexes asserts the base pack's own numbering."""
        remap = {}  # old identity hash -> new identity hash
        for name, spv in sorted(files.items()):
            if not name.endswith('.spv'):
                continue
            program = int(name[2:18], 16)
            old_index = int(name.split('_')[1].split('.')[0])
            identity = hash_combine(program, old_index)
            meta = files.get(f'0x{identity:016x}.meta')
            if meta is None or len(meta) < 24 or len(spv) % 4 or \
                    struct.unpack_from('<I', spv)[0] != SPIRV_MAGIC:
                self.count(source, 'skip_unpaired_or_bad_spirv')
                continue
            version, binary, meta_hash, meta_index = struct.unpack_from('<IIQQ', meta)
            if version not in META_VERSIONS or binary != BINARY_VERSION or \
                    meta_hash != identity or meta_index != old_index:
                self.count(source, f'skip_meta_v{version}_b{binary}')
                continue
            body = meta[24:]
            content = (program, sha(body), sha(spv))
            perms = self.perms.setdefault(program, [])
            # The base pack keeps every slot it has, even two slots with equal content.
            index = None if keep_indexes else self.content.get(content)
            if index is None:
                if keep_indexes:
                    # Names arrive in text order (_10 before _2): place each slot directly.
                    while len(perms) <= old_index:
                        perms.append(None)  # gap in the base pack, never referenced
                    assert perms[old_index] is None, ('base pack index clash', name)
                    index = old_index
                    perms[index] = (version, body, spv)
                else:
                    index = len(perms)
                    perms.append((version, body, spv))
                self.content.setdefault(content, index)
                self.count(source, 'perm_added' if old_index == index else 'perm_added_renumbered')
            else:
                self.count(source, 'perm_shared')
            remap[identity] = hash_combine(program, index)
        for name, data in sorted(files.items()):
            if not name.endswith('.key'):
                continue
            if len(data) < 16:
                self.count(source, 'skip_key_short')
                continue
            version, compute = struct.unpack_from('<II', data)
            if version != KEY_VERSION:
                self.count(source, f'skip_key_v{version}')
                continue
            if compute:
                if len(data) != 16:
                    self.count(source, 'skip_compute_key_size')
                    continue
                (stage,) = struct.unpack_from('<Q', data, 8)
                if stage not in remap:
                    self.count(source, 'skip_key_missing_stage')
                    continue
                new = bytearray(data)
                struct.pack_into('<Q', new, 8, remap[stage])
            else:
                if len(data) < self.graphics_key_size:
                    self.count(source, 'skip_graphics_key_size')
                    continue
                stages = struct.unpack_from('<6Q', data, 8)
                if any(h and h not in remap for h in stages):
                    self.count(source, 'skip_key_missing_stage')
                    continue
                new = bytearray(data)
                struct.pack_into('<6Q', new, 8, *[remap[h] if h else 0 for h in stages])
            new = bytes(new)
            if new in self.keys:
                self.count(source, 'key_shared')
            else:
                self.keys[new] = 'c' if compute else 'g'
                self.count(source, 'key_added')

    def files(self):
        out = {}
        variants = set()
        for program, perms in self.perms.items():
            for index, perm in enumerate(perms):
                if perm is None:
                    continue
                version, body, spv = perm
                identity = hash_combine(program, index)
                assert identity not in variants, 'colliding permutation hash'
                variants.add(identity)
                out[f'0x{identity:016x}.meta'] = (
                    struct.pack('<IIQQ', version, BINARY_VERSION, identity, index) + body)
                out[f'0x{program:016x}_{index}.spv'] = spv
        for key, kind in self.keys.items():
            if kind == 'c':
                (stage,) = struct.unpack_from('<Q', key, 8)
                out[f'c_0x{stage:016x}.key'] = key
            else:
                out[f'g_0x{int(sha(key)[:16], 16):016x}.key'] = key
        return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--base', type=Path, default=ROOT / 'out/pipeline-library/CUSA00004-packed')
    p.add_argument('--lineage', type=Path, action='append', default=[],
                   help='cache directory holding profile.bin/.meta/.spv/.key (CUSA00004 folder)')
    p.add_argument('--output', type=Path)
    p.add_argument('--dry-run', action='store_true')
    a = p.parse_args()

    base_files, profile = read_pack(a.base)
    # Same minimum as pipeline_library.cpp (GraphicsKeyMinBytes): key struct + variable sdata.
    merged = Merged(min(len(d) for n, d in base_files.items() if n.startswith('g_')))
    sources = [('base', base_files, True)]
    skipped = []
    for lineage in a.lineage:
        lineage = lineage.resolve()
        prof = lineage / 'profile.bin'
        if not prof.is_file() or prof.read_bytes() != profile:
            skipped.append(str(lineage))
            continue
        sources.append((str(lineage), read_dir(lineage), False))

    for source, files, keep in sources:
        merged.add_lineage(source, files, keep)

    out_files = merged.files()
    n_compute = sum(n.startswith('c_') for n in out_files)
    n_graphics = sum(n.startswith('g_') for n in out_files)
    n_programs = len(merged.perms)
    summary = dict(programs=n_programs, permutations=sum(n.endswith('.spv') for n in out_files),
                   compute_keys=n_compute, graphics_keys=n_graphics,
                   bytes=sum(len(d) for d in out_files.values()),
                   lineages_used=len(sources) - 1, lineages_skipped_profile=skipped)
    print(json.dumps(summary, indent=2))
    print(json.dumps(merged.stats, indent=2))
    if a.dry_run:
        return 0
    assert a.output, '--output required'
    out = a.output.resolve()
    assert out.is_relative_to((ROOT / 'out/experimental').resolve()), 'output must be experimental'
    out.mkdir(parents=True, exist_ok=False)
    assets, blob = [], bytearray()
    for name, data in sorted(out_files.items()):
        assets.append(dict(filename=name, offset=len(blob), size=len(data), sha256=sha(data)))
        blob.extend(data)
    (out / 'recipes.bin').write_bytes(blob)
    (out / 'profile.bin').write_bytes(profile)
    manifest = dict(schema=1, profile=dict(filename='profile.bin', size=len(profile), sha256=sha(profile)),
                    pack=dict(filename='recipes.bin', size=len(blob), sha256=sha(blob)), assets=assets)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    selection = dict(schema='iss.shader-merged.v1', base=str(a.base.resolve()),
                     base_manifest_sha256=sha((a.base / 'manifest.json').read_bytes()),
                     summary=summary, per_source=merged.stats,
                     limitations=['Only observed variants; unseen shaders still compile.',
                                  'Permutation indexes renumbered by content; base pack indexes kept.',
                                  'SPIR-V from lineages with the same binary version is trusted as-is.',
                                  'No driver binary: portable only to a matching Shader::Profile.'])
    (out / 'selection.json').write_text(json.dumps(selection, indent=2), encoding='utf-8')
    print('assets', len(assets), 'bytes', len(blob), 'sha256', sha(blob))
    return 0


if __name__ == '__main__':
    sys.exit(main())
