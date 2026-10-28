"""Create a portable, profile-bound library from one shader cache directory.

Only the emulator's key, metadata and SPIR-V files are copied. The opaque Vulkan
driver cache next to the source directory is deliberately outside this format.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


KEY_NAME = re.compile(r"(?:c|g)_0x[0-9a-f]{16}\.key\Z")
META_NAME = re.compile(r"0x([0-9a-f]{16})\.meta\Z")
SPV_NAME = re.compile(r"0x([0-9a-f]{16})_([0-9]+)\.spv\Z")
MASK64 = (1 << 64) - 1
SPIRV_MAGIC = 0x07230203
# Header (8) + GraphicsPipelineKey (344) + fixed SerializationSupport (1240)
# in the current MSVC x64 cache ABI. Variable tessellation vectors may add data.
GRAPHICS_KEY_MIN_BYTES = 1592


class InvalidLibrary(ValueError):
    pass


def hash_combine(seed: int, index: int) -> int:
    return (seed ^ ((index + 0x9E3779B9 + ((seed << 12) & MASK64) + (seed >> 4)) & MASK64)) & MASK64


def _read_regular_file(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise InvalidLibrary(f"not a regular file: {path}")
    return path.read_bytes()


def _entry(name: str, data: bytes) -> dict[str, str | int]:
    return {"filename": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def _spirv_compute_entrypoint(data: bytes) -> bool:
    """True only for a well-formed instruction stream with one GLCompute entrypoint."""
    if len(data) < 20 or len(data) % 4 or struct.unpack_from("<I", data)[0] != SPIRV_MAGIC:
        raise InvalidLibrary("invalid SPIR-V header")
    models: list[int] = []
    offset = 20
    while offset < len(data):
        first = struct.unpack_from("<I", data, offset)[0]
        words, opcode = first >> 16, first & 0xFFFF
        end = offset + words * 4
        if words == 0 or end > len(data):
            raise InvalidLibrary("invalid SPIR-V instruction bounds")
        if opcode == 15:  # OpEntryPoint
            if words < 4 or b"\x00" not in data[offset + 12:end]:
                raise InvalidLibrary("invalid SPIR-V OpEntryPoint")
            models.append(struct.unpack_from("<I", data, offset + 4)[0])
        offset = end
    return models == [5]  # ExecutionModel GLCompute


def _collect(source: Path, skip_incomplete: bool, recover_compute: bool) -> tuple[dict, dict[str, bytes]]:
    if source.is_symlink() or not source.is_dir():
        raise InvalidLibrary(f"source is not a directory: {source}")
    files: dict[str, bytes] = {}
    for path in source.iterdir():
        if path.name == "profile.bin":
            continue
        if path.suffix not in {".key", ".meta", ".spv"}:
            continue
        if not (KEY_NAME.fullmatch(path.name) or META_NAME.fullmatch(path.name) or SPV_NAME.fullmatch(path.name)):
            raise InvalidLibrary(f"invalid cache filename: {path.name}")
        files[path.name] = _read_regular_file(path)

    profile = _read_regular_file(source / "profile.bin")
    if not profile:
        raise InvalidLibrary("empty profile.bin")

    # Every SPIR-V module must have matching versioned metadata. In the cache,
    # the metadata name is HashCombine(base shader hash, permutation index).
    variants: dict[int, tuple[str, int]] = {}
    rejected: list[dict[str, str]] = []
    for name, data in list(files.items()):
        match = SPV_NAME.fullmatch(name)
        if not match:
            continue
        if len(data) < 20 or len(data) % 4 or struct.unpack_from("<I", data)[0] != SPIRV_MAGIC:
            raise InvalidLibrary(f"invalid SPIR-V: {name}")
        shader_hash, permutation = int(match[1], 16), int(match[2])
        if match[2] != str(permutation) or permutation > MASK64:
            raise InvalidLibrary(f"noncanonical permutation index: {name}")
        variant_hash = hash_combine(shader_hash, permutation)
        if variant_hash in variants:
            raise InvalidLibrary(f"colliding variant hash: {name} and {variants[variant_hash][0]}")
        variants[variant_hash] = (name, permutation)

    for name, data in files.items():
        match = META_NAME.fullmatch(name)
        if not match:
            continue
        if len(data) < 24:
            raise InvalidLibrary(f"short shader metadata: {name}")
        meta_version, binary_version, stored_hash, permutation = struct.unpack_from("<IIQQ", data)
        variant_hash = int(match[1], 16)
        variant = variants.get(variant_hash)
        # The current port writes v7 metadata with captured SRT relocation data and
        # still loads the v6 legacy pack. Keep the offline gate aligned with the
        # loader; it must not reject newly observed, supported recipes.
        if (meta_version not in (6, 7) or binary_version < 8 or binary_version % 100 != 8
                or stored_hash != variant_hash or variant is None or permutation != variant[1]):
            raise InvalidLibrary(f"incompatible or unpaired shader metadata: {name}")
    for variant_hash, (name, _) in variants.items():
        if f"0x{variant_hash:016x}.meta" not in files:
            raise InvalidLibrary(f"SPIR-V has no metadata: {name}")

    recovered_compute: list[dict[str, str]] = []
    if recover_compute:
        for variant_hash, (spv_name, _) in sorted(variants.items()):
            key_name = f"c_0x{variant_hash:016x}.key"
            if key_name in files:
                continue
            try:
                is_compute = _spirv_compute_entrypoint(files[spv_name])
            except InvalidLibrary as exc:
                raise InvalidLibrary(f"{spv_name}: {exc}") from exc
            if not is_compute:
                continue
            files[key_name] = struct.pack("<IIQ", 3, 1, variant_hash)
            recovered_compute.append({"filename": key_name, "meta": f"0x{variant_hash:016x}.meta", "spv": spv_name})

    for name, data in list(files.items()):
        if not KEY_NAME.fullmatch(name):
            continue
        if len(data) < 16 or struct.unpack_from("<I", data)[0] != 3:
            raise InvalidLibrary(f"invalid pipeline key header: {name}")
        kind = struct.unpack_from("<I", data, 4)[0]
        if name.startswith("c_"):
            if kind != 1 or len(data) != 16:
                raise InvalidLibrary(f"invalid compute key: {name}")
            variant_hash = struct.unpack_from("<Q", data, 8)[0]
            if name != f"c_0x{variant_hash:016x}.key":
                raise InvalidLibrary(f"compute key filename differs from bytes: {name}")
            if f"0x{variant_hash:016x}.meta" not in files:
                if not skip_incomplete:
                    raise InvalidLibrary(f"compute key has no shader variant: {name}")
                rejected.append({"filename": name, "reason": "missing shader variant"})
                del files[name]
        else:
            if kind != 0 or len(data) < GRAPHICS_KEY_MIN_BYTES:
                raise InvalidLibrary(f"invalid graphics key: {name}")
            # stage_hashes is the first field of GraphicsPipelineKey in this ABI.
            if any(variant_hash and f"0x{variant_hash:016x}.meta" not in files
                   for variant_hash in struct.unpack_from("<6Q", data, 8)):
                if not skip_incomplete:
                    raise InvalidLibrary(f"graphics key has no shader variant: {name}")
                rejected.append({"filename": name, "reason": "missing shader variant"})
                del files[name]

    manifest = {
        "schema": 1,
        "profile": _entry("profile.bin", profile),
        "assets": [_entry(name, files[name]) for name in sorted(files)],
        "rejected": sorted(rejected, key=lambda item: item["filename"]),
        "recovered_compute": recovered_compute,
    }
    files["profile.bin"] = profile
    return manifest, files


def prepare(source: Path, output: Path, *, skip_incomplete: bool = False,
            recover_compute: bool = False, packed: bool = False) -> dict:
    if source.is_symlink():
        raise InvalidLibrary(f"source cannot be a symlink: {source}")
    source = source.resolve(strict=True)
    if output.is_symlink():
        raise InvalidLibrary(f"output cannot be a symlink: {output}")
    # Resolve existing parent symlinks before checking directory separation.
    output = output.resolve(strict=False)
    if output.exists() or output.is_symlink():
        raise InvalidLibrary(f"output already exists: {output}")
    if output == source or source in output.parents or output in source.parents:
        raise InvalidLibrary("source and output directories must be separate")
    manifest, files = _collect(source, skip_incomplete, recover_compute)
    if packed:
        digest = hashlib.sha256()
        offset = 0
        for item in manifest["assets"]:
            item["offset"] = offset
            data = files[item["filename"]]
            digest.update(data)
            offset += len(data)
        manifest["pack"] = {"filename": "recipes.bin", "size": offset, "sha256": digest.hexdigest()}
    output.mkdir(parents=True, exist_ok=False)
    if packed:
        (output / "profile.bin").write_bytes(files["profile.bin"])
        with (output / "recipes.bin").open("wb") as pack:
            for item in manifest["assets"]:
                pack.write(files[item["filename"]])
    else:
        for name in sorted(files):
            (output / name).write_bytes(files[name])
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="one cache directory containing profile.bin")
    parser.add_argument("--output", type=Path, required=True, help="new library directory")
    parser.add_argument("--skip-incomplete", action="store_true", help="omit keys lacking a shader variant and record them in manifest.json")
    parser.add_argument("--recover-compute", action="store_true", help="add missing compute keys for complete single-entrypoint GLCompute variants")
    parser.add_argument("--packed", action="store_true", help="store assets contiguously in recipes.bin with manifest offsets")
    args = parser.parse_args()
    manifest = prepare(args.source, args.output, skip_incomplete=args.skip_incomplete,
                       recover_compute=args.recover_compute, packed=args.packed)
    print(f"Prepared {len(manifest['assets'])} cache assets in {args.output}; "
          f"{len(manifest['rejected'])} rejected keys; "
          f"{len(manifest['recovered_compute'])} recovered compute keys")


if __name__ == "__main__":
    main()
