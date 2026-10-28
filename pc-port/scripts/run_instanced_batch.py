"""Isolated A/B runner for the two approved-pack instanced batch keys.

Both control and candidate enable drawIndirectFirstInstance on the logical
device. Only --batch records the new path. Never uses experimental shader seeds.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path

import run_probe


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out"
PACK = OUT / "pipeline-library/CUSA00004-packed"
TRUSTED = OUT / "pipeline-library-runtime-trusted/CUSA00004"
MANIFEST_SHA = "c3f63382830107fc0221c24dd3376533807a73357ec1407399531c1e9b815e66"
EXPECTED = {
    "0x000000001318ddd4_0.spv": "6e7100ca4c8bbaeaec52a74dddf50eadc87ac7ce7de24a28795358dbfe725147",
    "0x00000000c01162f7_0.spv": "4820ebbdc98e622259e7a67874eefb2eb438d939e0ea9445c034b8f4d68a3f7a",
    "0x0000000047b0694b_0.spv": "641db49e4dcdb476397e8b2505c4c7474e60c1182f117dbe3a6218ba74a96f7c",
    "0x00000c0100769d1f.meta": "5da97883a2b59b1ba1542d26e71d6b7a707865637f6ea9ca2e7e37aff3b3d281",
    "0x0000047beef75906.meta": "e4ae0dbf9c626a5b4a5a247b5ee604880ae93c7007da0060875e1b8c539eaef1",
    "g_0x3b6d3a6d0dec8019.key": "a708efb8ac3a3e036b28cea07e994f6e425e723e4161e08427f40ec9828f3474",
    "g_0x85c78d166865fbd7.key": "0a9458ad75d4d344c589dc14e6859ed2f3eb351aeb7439fd97e52a74211d8980",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def verify_pack_and_seed():
    manifest_bytes = (PACK / "manifest.json").read_bytes()
    if sha(manifest_bytes) != MANIFEST_SHA:
        raise ValueError("active approved pack manifest identity changed")
    manifest = json.loads(manifest_bytes)
    assets = {asset["filename"]: asset for asset in manifest["assets"]}
    with (PACK / manifest["pack"]["filename"]).open("rb") as packed:
        for name, expected in EXPECTED.items():
            asset = assets[name]
            if asset["sha256"] != expected:
                raise ValueError(f"pack manifest has wrong {name}")
            packed.seek(asset["offset"])
            if sha(packed.read(asset["size"])) != expected:
                raise ValueError(f"pack payload has wrong {name}")
            override = TRUSTED / name
            if override.exists() and sha(override.read_bytes()) != expected:
                raise ValueError(f"trusted seed overrides {name} with different bytes")
    return {"manifest_sha256": MANIFEST_SHA, "verified_assets": len(EXPECTED),
            "trusted_seed": str(TRUSTED)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True,
                        help="directory containing ABI-matched iss-probe.exe and iss-gpu.dll")
    parser.add_argument("--batch", action="store_true", help="record the instanced batch")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--diagnostic", action="store_true")
    args = parser.parse_args()
    release = args.release.resolve(strict=True)
    if not (release / "iss-probe.exe").is_file() or not (release / "iss-gpu.dll").is_file():
        parser.error("release needs an ABI-matched EXE and DLL")
    proof = verify_pack_and_seed()
    print("NATIVE_INSTANCED_PREFLIGHT", json.dumps(proof, sort_keys=True), flush=True)
    run_probe.apply_approved_environment(os.environ, OUT)
    os.environ.update({
        "ISS_PROBE_EXE": str(release / "iss-probe.exe"),
        "ISS_SAVE_SEED": str(OUT / "user/city-savedata"),
        "ISS_NATIVE_GEOMETRY": "0", "ISS_NATIVE_DRAW_STATE": "0",
        "ISS_NATIVE_DRAW_STATE_VERIFY": "0", "ISS_NATIVE_INDEXED_BATCH": "0",
        "ISS_NATIVE_INSTANCED_FEATURE": "1", "ISS_NATIVE_INSTANCED_BATCH": "1" if args.batch else "0",
        "ISS_SECONDARY_SPV_VERIFIED": "1", "ISS_SECONDARY_BATCH_PROBE": "0",
        "ISS_NATIVE_CITY_FETCH": "0", "ISS_PROFILE_CITY_DRAW_GPU": "0",
        "ISS_DRAW_PIPE": "0", "ISS_FEEDBACK_JOIN": "0",
        "ISS_NATIVE_MEMORY_REPORT": "1" if args.diagnostic else "0",
        "ISS_COMMAND_TRACE": "0", "ISS_GPU_PROFILE": "0", "ISS_DEEP_PROFILE": "0",
        "ISS_MINIMAL_LOGS": "0", # frames.csv for like-for-like present-time comparison
    })
    return run_probe.run(entry=True, linked=True, gpu=True, timeout=args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
