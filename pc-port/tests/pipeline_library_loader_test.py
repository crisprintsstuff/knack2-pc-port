"""Filesystem fixtures for the standalone C++ pipeline-library loader harness.

Set PIPELINE_LIBRARY_LOADER_TEST to the compiled pipeline_library_loader_test.exe.
"""

from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from prepare_pipeline_library import hash_combine, prepare  # noqa: E402


class LoaderTest(unittest.TestCase):
    def setUp(self) -> None:
        executable = os.environ.get("PIPELINE_LIBRARY_LOADER_TEST")
        if not executable or not Path(executable).is_file():
            self.skipTest("set PIPELINE_LIBRARY_LOADER_TEST to the compiled harness")
        self.executable = executable
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source = self.root / "source"
        source.mkdir()
        self.source = source
        self.profile = source / "profile.bin"
        self.profile.write_bytes(bytes(range(64)))
        shader = 0x1234
        variant = hash_combine(shader, 0)
        self.spv_name = f"0x{shader:016x}_0.spv"
        self.meta_name = f"0x{variant:016x}.meta"
        self.key_name = f"c_0x{variant:016x}.key"
        (source / self.spv_name).write_bytes(struct.pack("<5I", 0x07230203, 0x10000, 0, 1, 0))
        (source / self.meta_name).write_bytes(struct.pack("<IIQQ", 6, 8, variant, 0) + b"meta")
        (source / self.key_name).write_bytes(struct.pack("<IIQ", 3, 1, variant))
        self.library = self.root / "library"
        prepare(source, self.library)
        self.primary = self.root / "cold-primary"
        self.primary.mkdir()

    def check_open(self, expected: bool, profile: Path | None = None,
                   library: Path | None = None, reason: str | None = None) -> None:
        completed = subprocess.run(
            [self.executable, str(library or self.library), str(self.primary), str(profile or self.profile),
             "1" if expected else "0"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        if reason:
            self.assertIn(reason, completed.stderr)

    def packed(self) -> Path:
        output = self.root / "packed"
        prepare(self.source, output, packed=True)
        return output

    def test_cold_primary_and_matching_profile(self) -> None:
        self.check_open(True)

    def test_profile_mismatch(self) -> None:
        other = self.root / "other-profile.bin"
        other.write_bytes(b"different-profile")
        self.check_open(False, other)

    def test_asset_corruption(self) -> None:
        with (self.library / self.spv_name).open("ab") as stream:
            stream.write(b"corruption")
        self.check_open(False)

    def test_primary_filename_conflict(self) -> None:
        (self.primary / self.meta_name).write_bytes(b"different-metadata")
        self.check_open(False)

    def test_manifest_omitting_required_metadata(self) -> None:
        manifest_path = self.library / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["assets"] = [item for item in manifest["assets"] if item["filename"] != self.meta_name]
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        (self.library / self.meta_name).unlink()
        self.check_open(False)

    def test_packed_cold_primary(self) -> None:
        self.check_open(True, library=self.packed())

    def test_packed_data_corruption(self) -> None:
        packed = self.packed()
        with (packed / "recipes.bin").open("ab") as stream:
            stream.write(b"corruption")
        self.check_open(False, library=packed, reason="packed library integrity mismatch")

    def test_packed_overlapping_offsets(self) -> None:
        packed = self.packed()
        manifest_path = packed / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["assets"][1]["offset"] = manifest["assets"][0]["offset"]
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        self.check_open(False, library=packed, reason="noncontiguous packed recipes")

    def test_packed_truncated_bounds_with_updated_pack_digest(self) -> None:
        packed = self.packed()
        pack_path = packed / "recipes.bin"
        data = pack_path.read_bytes()[:-1]
        pack_path.write_bytes(data)
        manifest_path = packed / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["pack"]["size"] = len(data)
        manifest["pack"]["sha256"] = hashlib.sha256(data).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        self.check_open(False, library=packed, reason="packed recipe outside bounds")


if __name__ == "__main__":
    unittest.main()
