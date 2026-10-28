"""Audit existing approved-pack SPV, fetch metadata and keys for two draw families.

Read-only. Requires the Vulkan SDK's spirv-dis on PATH; never changes the pack.
"""

import argparse
import hashlib
import json
import mmap
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


VERTICES = {0xC01162F7: 7, 0x47B0694B: 4}
FRAGMENT = 0x1318DDD4
MASK64 = (1 << 64) - 1


def combine(base, permutation):
    return (base ^ (permutation + 0x9E3779B9 + (base << 12) + (base >> 4))) & MASK64


def asset_data(packed, asset):
    data = packed[asset["offset"]:asset["offset"] + asset["size"]]
    assert hashlib.sha256(data).hexdigest() == asset["sha256"]
    return data


def disassemble(data, executable):
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "shader.spv"
        path.write_bytes(data)
        proc = subprocess.run([executable, str(path), "--no-color"], capture_output=True,
                              text=True, check=True)
        return proc.stdout


def fetch_records(data):
    # StageSpecialization writes sizeof(FetchShaderData)==104 before serialized
    # size/s8 offsets/count/8-byte attributes. Require the exact one-attribute
    # layout rather than guessing from an arbitrary 104-byte match.
    marker = struct.pack("<Q", 104)
    result = []
    start = 0
    while (offset := data.find(marker, start)) >= 0:
        start = offset + 1
        if offset + 30 > len(data):
            continue
        size = struct.unpack_from("<I", data, offset + 8)[0]
        vertex_offset, instance_offset = struct.unpack_from("<bb", data, offset + 12)
        count = struct.unpack_from("<Q", data, offset + 14)[0]
        if size != 24 or count != 1 or abs(vertex_offset) > 32 or abs(instance_offset) > 32:
            continue
        attribute = list(data[offset + 22:offset + 30])
        if attribute[:4] != [4, 3, 2, 0]:
            continue
        result.append({"offset": offset + 8, "fetch_code_size": size,
                       "vertex_offset_sgpr": vertex_offset,
                       "instance_offset_sgpr": instance_offset,
                       "attribute": attribute})
    return result


def spirv_contract(text):
    builtins = re.findall(r"OpDecorate\s+%\S+\s+BuiltIn\s+(\w+)", text)
    instance_load = re.search(
        r"(?m)^\s*(%\w+)\s*=\s*OpLoad[^\n]*%gl_InstanceIndex\s*$", text)
    uses = []
    if instance_load:
        token = instance_load.group(1)
        uses = [line.strip() for line in text.splitlines()
                if re.search(re.escape(token) + r"(?=\s|$)", line)]
    return {
        "entry": next((line.strip() for line in text.splitlines()
                       if "OpEntryPoint" in line), None),
        "builtins": builtins,
        "vertex_index_loads": len(re.findall(r"OpLoad[^\n]*%gl_VertexIndex", text)),
        "instance_index_load_uses": uses,
        "instance_index_only_ssbo_72_byte_records": bool(instance_load) and
            len(uses) == 5 and all("OpIMul" in line and "%u32_id_18" in line
                                   for line in uses[1:]) and
            "%u32_id_18 = OpConstant %u32_id 18" in text,
        "push_data_loads": len(re.findall(r"Op(?:Load|AccessChain)[^\n]*%push_data", text)),
        "array_length_ops": text.count("OpArrayLength"),
        "ssbo_1_access_chains": len(re.findall(r"OpAccessChain[^\n]*%ssbo_1", text)),
        "clip_planes_access_chains": len(re.findall(r"OpAccessChain[^\n]*%clip_planes", text)),
    }


def analyze(pack):
    executable = shutil.which("spirv-dis")
    if not executable:
        raise RuntimeError("spirv-dis from the Vulkan SDK is required")
    manifest = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
    assets = {a["filename"]: a for a in manifest["assets"]}
    report = {"schema": "iss.secondary-batch-static.v1", "pack": str(pack.resolve()),
              "manifest_sha256": hashlib.sha256((pack / "manifest.json").read_bytes()).hexdigest(),
              "vertices": {}, "fragment": {}, "graphics_keys": []}
    with (pack / manifest["pack"]["filename"]).open("rb") as source:
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as packed:
            for base, variants in VERTICES.items():
                rows = []
                for perm in range(variants):
                    spv_name = f"0x{base:016x}_{perm}.spv"
                    meta_name = f"0x{combine(base, perm):016x}.meta"
                    spv = asset_data(packed, assets[spv_name])
                    meta = asset_data(packed, assets[meta_name])
                    rows.append({"permutation": perm, "spv_sha256": assets[spv_name]["sha256"],
                                 "meta_sha256": assets[meta_name]["sha256"],
                                 "permhash": f"0x{combine(base, perm):016x}",
                                 "fetch_candidates": fetch_records(meta),
                                 "spirv": spirv_contract(disassemble(spv, executable))})
                report["vertices"][f"0x{base:08x}"] = rows
            name = f"0x{FRAGMENT:016x}_0.spv"
            fragment_spv = asset_data(packed, assets[name])
            report["fragment"] = {"hash": f"0x{FRAGMENT:08x}",
                                  "spv_sha256": assets[name]["sha256"],
                                  "spirv": spirv_contract(disassemble(fragment_spv, executable))}
            vertex_permhashes = {
                combine(base, perm): (base, perm)
                for base, variants in VERTICES.items() for perm in range(variants)
            }
            fragment_permhash = combine(FRAGMENT, 0)
            for name, asset in assets.items():
                if not name.endswith(".key"):
                    continue
                data = asset_data(packed, asset)
                version, kind = struct.unpack_from("<II", data)
                if version != 3 or kind != 0:
                    continue
                key = data[8:352]
                stage_hashes = struct.unpack_from("<6Q", key)
                if stage_hashes[0] != fragment_permhash or stage_hashes[3] not in vertex_permhashes:
                    continue
                base, perm = vertex_permhashes[stage_hashes[3]]
                report["graphics_keys"].append({
                    "key": name, "vertex_hash": f"0x{base:08x}", "vertex_permutation": perm,
                    "primitive_raw": f"0x{struct.unpack_from('<I', key, 340)[0]:08x}",
                    "vertex_formats_first4": list(struct.unpack_from("<4I", key, 48)),
                })
    report["graphics_keys"].sort(key=lambda row: row["key"])
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    content = json.dumps(analyze(args.pack), indent=2) + "\n"
    if args.output:
        args.output.write_text(content, encoding="utf-8")
    print(content)
