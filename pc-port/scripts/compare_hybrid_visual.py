"""Exact, local comparison of GameOnly PNGs at explicit game-frame ordinals.

Manifest: {"frames": {"1234": "capture.png"},
           "provenance": {"build_hashes": {"exe": "sha256", "dll": "sha256"}}}
Paths are relative to their manifest. A plain frame-to-path mapping is also accepted.
Exit codes: 0 identical, 1 unequal (cause UNKNOWN), 2 invalid inputs/dependencies.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile


def dependencies():
    try:
        from PIL import Image
        import numpy as np
    except ImportError as exc:
        raise ValueError(f"Local Pillow and numpy are required; nothing was installed: {exc}") from exc
    return Image, np


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_manifest(path):
    path = Path(path).resolve()
    data = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_object)
    if not isinstance(data, dict):
        raise ValueError(f"Manifest must be a JSON object: {path}")
    frames = data.get("frames", data)
    if not isinstance(frames, dict) or not frames:
        raise ValueError(f"An explicit nonempty frame mapping is required: {path}")
    normalized = {}
    for key, value in frames.items():
        if not key.isdecimal() or int(key) < 0 or int(key) in normalized:
            raise ValueError(f"Invalid/duplicate frame ordinal {key!r}: {path}")
        if isinstance(value, dict):
            value = value.get("path")
        if not isinstance(value, str) or not value:
            raise ValueError(f"Frame {key} requires a PNG path: {path}")
        image_path = Path(value)
        if not image_path.is_absolute():
            image_path = path.parent / image_path
        normalized[int(key)] = image_path.resolve()
    provenance = data.get("provenance", {}) if "frames" in data else {}
    if not isinstance(provenance, dict):
        raise ValueError("provenance must be an object")
    hashes = provenance.get("build_hashes", data.get("build_hashes") if "frames" in data else None)
    if hashes is not None and (not isinstance(hashes, dict) or not hashes or
                               any(not isinstance(v, str) or not v for v in hashes.values())):
        raise ValueError("build_hashes must be a nonempty object of artifact names to hashes")
    return path, normalized, hashes


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pixels(path, Image, np):
    encoded = path.read_bytes()
    with Image.open(path) as image:
        if image.format != "PNG" or getattr(image, "n_frames", 1) != 1:
            raise ValueError(f"Expected a single-frame PNG: {path}")
        # GameOnly captures are eight-bit RGB/RGBA. Reject other precision/modes:
        # silently converting 16-bit data could conceal a difference.
        if image.mode not in ("RGB", "RGBA"):
            raise ValueError(f"Expected eight-bit RGB/RGBA, got {image.mode}: {path}")
        array = np.asarray(image.convert("RGBA"), dtype=np.uint8).copy()
        return array, {"path": str(path), "size": list(image.size), "mode": image.mode,
                       "png_sha256": sha256(encoded), "rgba_sha256": sha256(array.tobytes())}


def compare(off_manifest, on_manifest):
    Image, np = dependencies()
    off_path, off, off_hashes = read_manifest(off_manifest)
    on_path, on, on_hashes = read_manifest(on_manifest)
    if set(off) != set(on):
        raise ValueError(f"Frame sets differ: only_off={sorted(set(off)-set(on))}, "
                         f"only_on={sorted(set(on)-set(off))}")
    if off_hashes != on_hashes:
        raise ValueError("Build provenance hashes differ or are supplied on only one side")
    results = []
    total_changed = total_pixels = total_error = total_channels = maximum = 0
    for frame in sorted(off):
        a, ameta = pixels(off[frame], Image, np)
        b, bmeta = pixels(on[frame], Image, np)
        if a.shape != b.shape:
            raise ValueError(f"Image sizes differ at frame {frame}: {ameta['size']} vs {bmeta['size']}")
        delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
        changed = int(np.count_nonzero(np.any(delta != 0, axis=2)))
        count = int(a.shape[0] * a.shape[1])
        error = int(delta.sum(dtype=np.uint64))
        peak = int(delta.max())
        results.append({"frame": frame, "off": ameta, "on": bmeta,
                        "exact_pixel_equality": changed == 0, "changed_pixels": changed,
                        "pixels": count, "max_absolute_error": peak,
                        "mean_absolute_error_rgba": error / a.size,
                        "difference_cause": "NONE" if changed == 0 else "UNKNOWN"})
        total_changed += changed
        total_pixels += count
        total_error += error
        total_channels += a.size
        maximum = max(maximum, peak)
    equal = total_changed == 0
    return {"status": "EXACT_EQUAL" if equal else "UNEQUAL_CAUSE_UNKNOWN",
            "exact_pixel_equality": equal, "off_manifest": str(off_path),
            "on_manifest": str(on_path), "build_hashes": off_hashes,
            "same_build_provenance_supplied": off_hashes is not None,
            "frame_count": len(results), "changed_pixels": total_changed,
            "pixels": total_pixels, "max_absolute_error": maximum,
            "mean_absolute_error_rgba": total_error / total_channels,
            "interpretation": ("All compared RGBA samples are exactly equal; this is not proof for unmeasured frames."
                               if equal else "Pixels differ exactly as reported. Nondeterministic scene differences "
                               "and renderer changes cannot be distinguished by these captures; cause UNKNOWN."),
            "frames": results}


def self_test():
    Image, np = dependencies()
    with tempfile.TemporaryDirectory(prefix="hybrid-visual-test-") as directory:
        root = Path(directory)
        data = np.zeros((2, 3, 4), dtype=np.uint8)
        data[:, :, 3] = 255
        Image.fromarray(data).save(root / "off.png")
        Image.fromarray(data).save(root / "on.png")
        for side in ("off", "on"):
            (root / f"{side}.json").write_text(json.dumps({"frames": {"17": f"{side}.png"},
                "provenance": {"build_hashes": {"exe": "test"}}}), encoding="utf-8")
        a, b = root / "off.json", root / "on.json"
        assert compare(a, b)["exact_pixel_equality"]
        data[0, 0, 0] = 7
        Image.fromarray(data).save(root / "on.png")
        result = compare(a, b)
        assert not result["exact_pixel_equality"] and result["changed_pixels"] == 1
        assert result["max_absolute_error"] == 7
        assert result["mean_absolute_error_rgba"] == 7 / 24
        Image.new("RGBA", (4, 2)).save(root / "on.png")
        try:
            compare(a, b)
        except ValueError:
            pass
        else:
            raise AssertionError("Different dimensions were not rejected")
    return {"status": "SELF_TEST_PASS", "checks": ["equal", "one_pixel", "different_sizes"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("off", nargs="?")
    parser.add_argument("on", nargs="?")
    parser.add_argument("--output", type=Path, help="Write the JSON report to a new file")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            result = self_test()
        else:
            if not args.off or not args.on:
                parser.error("off and on manifests are required unless --self-test is used")
            result = compare(args.off, args.on)
        code = 0 if result.get("exact_pixel_equality", True) else 1
    except (ValueError, OSError, AssertionError) as exc:
        result, code = {"status": "INVALID", "error": str(exc)}, 2
    report = json.dumps(result, indent=2, ensure_ascii=False)
    if args.output:
        # Avoid overwriting captures, manifests or earlier evidence by accident.
        with args.output.open("x", encoding="utf-8") as output:
            output.write(report + "\n")
    print(report)
    return code


if __name__ == "__main__":
    sys.exit(main())
