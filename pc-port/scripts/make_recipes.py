"""Pipeline recipes from a shader library (Claude, 2026-10-19).

python scripts/make_recipes.py <library_dir> <out_dir> <verified.bin> [attribs.bin]

A recipe pack is the library without any shader code: the graphics pipeline keys (.key) and the
stage metadata (.meta: specialization, runtime info, resource layout), no SPIR-V. The port makes
the SPIR-V from the player's own game files when it loads them (ISS_AOT_RECIPES=1, ISS_AOT_INDEX,
gpu/overrides/vk_pipeline_cache.cpp RegenerateRecipeSpirv). Compute recipes are left out: compute
shaders are compiled ahead of time from the game files without recipes (ISS_AOT_COMPUTE).
Only stages whose SPIR-V made from the game files was verified identical to the library's
(verified.bin, ISS_AOT_RECIPE_VERIFIED) are kept, and only the pipelines whose stages all are:
a stage the specialization cannot tell apart from a wrong one must never be made.
The optional attribs.bin (ISS_AOT_RECIPE_ATTRS) is copied along: per vertex stage, the vertex
attribute formats its specialization does not keep."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import package_shader_merged_library as m  # noqa: E402


def is_compute(name, data):
    if name.startswith('c_'):
        return True
    if name.endswith('.meta') and len(data) >= 36:
        return struct.unpack_from('<I', data, 32)[0] == 5  # RuntimeInfo::sw_stage == Compute
    return False


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    files, profile = m.read_pack(src)
    raw = Path(sys.argv[3]).read_bytes()
    verified = set(struct.unpack('<%dQ' % (len(raw) // 8), raw))
    keep = {}
    for n, b in files.items():
        if n.endswith('.spv') or is_compute(n, b):
            continue
        if n.endswith('.meta') and struct.unpack_from('<Q', b, 8)[0] not in verified:
            continue
        if n.startswith('g_'):
            stages = struct.unpack_from('<6Q', b, 8)
            if any(h and h not in verified for h in stages):
                continue
        keep[n] = b
    dst.mkdir(parents=True, exist_ok=False)
    assets, digest, offset = [], hashlib.sha256(), 0
    with (dst / 'recipes.bin').open('wb') as f:
        for n, b in sorted(keep.items()):
            f.write(b)
            digest.update(b)
            assets.append(dict(filename=n, offset=offset, size=len(b), sha256=hashlib.sha256(b).hexdigest()))
            offset += len(b)
    (dst / 'profile.bin').write_bytes(profile)
    manifest = dict(schema=1,
                    profile=dict(filename='profile.bin', size=len(profile), sha256=hashlib.sha256(profile).hexdigest()),
                    pack=dict(filename='recipes.bin', size=offset, sha256=digest.hexdigest()),
                    assets=assets)
    (dst / 'manifest.json').write_text(json.dumps(manifest, separators=(',', ':')), encoding='utf-8')
    if len(sys.argv) > 4:
        shutil.copy2(sys.argv[4], dst / 'attribs.bin')
    keys = sum(n.endswith('.key') for n in keep)
    metas = sum(n.endswith('.meta') for n in keep)
    print(f'recipes: {keys} graphics pipelines, {metas} stages, {offset / 1e6:.1f} MB (no shader code); '
          f'left out {sum(n.endswith(".spv") for n in files)} SPIR-V and the compute recipes')


if __name__ == '__main__':
    main()
