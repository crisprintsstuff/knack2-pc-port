"""Package the validated 88-key first-use seed as an isolated experiment.

Does not change the approved executable, launcher, packed library or trusted cache.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
RESEARCH = WORKSPACE.parent / "Infamous Second Son Pipeline Research"
DEFAULT_SOURCE = RESEARCH / "artifacts/selected-first-use-927-936-20261013"
DEFAULT_OUTPUT = ROOT / "out/experimental/first-use-88"
DEFAULT_RELEASE = ROOT / "out/releases/20261013-first-use-88"
SOURCE_RELEASE = ROOT / "out/releases/20261013-renderer-map"
PACK = ROOT / "out/pipeline-library/CUSA00004-packed"
ALLOWED = {".key", ".meta", ".spv"}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def manifest_for(source: Path, source_release: Path) -> dict:
    selection_path = source / "selection.json"
    selection = json.loads(selection_path.read_text(encoding="utf-8"))
    assert selection["schema"] == "iss.selective-first-use-cache.v1"
    assert selection["selected_keys"] > 0 and len(selection["keys"]) == selection["selected_keys"]
    assert selection["frames"] and all(frame > 0 for frame in selection["frames"])
    assert digest(PACK / "manifest.json") == selection["pack_manifest_sha256"]
    cache = source / "CUSA00004"
    assert cache.is_dir() and not cache.is_symlink()
    assert digest(cache / "profile.bin") == selection["profile_sha256"]
    selected_names = {row["file"] for row in selection["keys"]}
    assert len(selected_names) == selection["selected_keys"]
    assert all((cache / name).is_file() for name in selected_names)
    assets = []
    for path in sorted(cache.iterdir()):
        if path.name == "profile.bin":
            pass
        elif path.suffix not in ALLOWED:
            raise ValueError(f"unexpected cache asset: {path.name}")
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"cache asset is not a regular file: {path}")
        assets.append({"filename": path.name, "size": path.stat().st_size, "sha256": digest(path)})
    total_keys = len([item for item in assets if item["filename"].endswith(".key")])
    assert total_keys >= selection["selected_keys"]
    exe = source_release / "iss-probe.exe"
    dll = source_release / "iss-gpu.dll"
    return {
        "schema": "iss.experimental-first-use-seed.v1",
        "title": f"{selection['selected_keys']} exact first-use keys from guest frame counters {selection['frames']}",
        "source_selection_sha256": digest(selection_path),
        "validated_manifest_sha256": selection["validated_manifest_sha256"],
        "pack_manifest_sha256": selection["pack_manifest_sha256"],
        "profile_sha256": selection["profile_sha256"],
        "selected_guest_frames": selection["frames"],
        "selected_keys": selection["selected_keys"],
        "total_keys": total_keys,
        "control_log": selection["control_log"],
        "assets": assets,
        "release": {"exe_sha256": digest(exe), "dll_sha256": digest(dll)},
        "limits": [
            "Experimental seed for exact keys observed in a prior session; new keys still compile synchronously.",
            "The active packed library must be present and match pack_manifest_sha256.",
            "This bundle does not replace the approved launcher, trusted cache or save.",
        ],
    }


def verify(output: Path, release: Path) -> dict:
    bundle = json.loads((output / "bundle.json").read_text(encoding="utf-8"))
    assert bundle["schema"] == "iss.experimental-first-use-seed.v1"
    assert digest(output / "selection.json") == bundle["source_selection_sha256"]
    selection = json.loads((output / "selection.json").read_text(encoding="utf-8"))
    assert bundle["selected_keys"] == selection["selected_keys"]
    assert bundle.get("selected_guest_frames", selection["frames"]) == selection["frames"]
    assert digest(PACK / "manifest.json") == bundle["pack_manifest_sha256"]
    cache = output / "CUSA00004"
    expected = {item["filename"] for item in bundle["assets"]}
    actual = {path.name for path in cache.iterdir() if path.is_file()}
    assert actual == expected
    assert len([name for name in actual if name.endswith(".key")]) == bundle["total_keys"]
    for item in bundle["assets"]:
        path = cache / item["filename"]
        assert path.is_file() and not path.is_symlink()
        assert path.stat().st_size == item["size"] and digest(path) == item["sha256"]
    assert digest(release / "iss-probe.exe") == bundle["release"]["exe_sha256"]
    assert digest(release / "iss-gpu.dll") == bundle["release"]["dll_sha256"]
    return bundle


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--release-output", type=Path, default=DEFAULT_RELEASE)
    parser.add_argument("--source-release", type=Path, default=SOURCE_RELEASE)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    output = args.output.resolve(strict=False)
    release = args.release_output.resolve(strict=False)
    source_release = args.source_release.resolve(strict=True)
    if not args.verify_only:
        if output.exists() or release.exists():
            raise FileExistsError("experimental output already exists; use --verify-only")
        bundle = manifest_for(source, source_release)
        shutil.copytree(source / "CUSA00004", output / "CUSA00004")
        shutil.copy2(source / "selection.json", output / "selection.json")
        (output / "bundle.json").write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
        release.mkdir(parents=True)
        shutil.copy2(source_release / "iss-probe.exe", release / "iss-probe.exe")
        shutil.copy2(source_release / "iss-gpu.dll", release / "iss-gpu.dll")
    bundle = verify(output, release)
    print(json.dumps({"bundle": str(output), "release": str(release),
                      "assets_verified": len(bundle["assets"]),
                      "selected_keys": bundle["selected_keys"], "total_keys": bundle["total_keys"],
                      "exe_sha256": bundle["release"]["exe_sha256"],
                      "dll_sha256": bundle["release"]["dll_sha256"]}, indent=2))


if __name__ == "__main__":
    main()
