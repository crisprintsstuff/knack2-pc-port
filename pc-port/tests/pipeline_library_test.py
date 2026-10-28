"""Isolated tests for the profile-bound pipeline library builder."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from prepare_pipeline_library import InvalidLibrary, hash_combine, prepare  # noqa: E402


def entrypoint(model: int) -> bytes:
    return struct.pack("<5I", (5 << 16) | 15, model, 1, 0x6E69616D, 0)


class PipelineLibraryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.shader_hash = 0x1234
        self.permutation = 0
        self.variant = hash_combine(self.shader_hash, self.permutation)
        self.spv_name = f"0x{self.shader_hash:016x}_{self.permutation}.spv"
        self.meta_name = f"0x{self.variant:016x}.meta"
        self.key_name = f"c_0x{self.variant:016x}.key"
        (self.source / "profile.bin").write_bytes(bytes(range(64)))
        (self.source / self.spv_name).write_bytes(struct.pack("<5I", 0x07230203, 0x10000, 0, 1, 0))
        (self.source / self.meta_name).write_bytes(struct.pack("<IIQQ", 6, 8, self.variant, 0) + b"metadata")
        (self.source / self.key_name).write_bytes(struct.pack("<IIQ", 3, 1, self.variant))

    def test_complete_library_is_deterministic_and_excludes_driver_cache(self) -> None:
        (self.source / "CUSA00004.vkcache").write_bytes(b"driver-specific")
        first = self.root / "library-one"
        second = self.root / "library-two"
        manifest = prepare(self.source, first)
        prepare(self.source, second)
        self.assertEqual((first / "manifest.json").read_bytes(), (second / "manifest.json").read_bytes())
        self.assertEqual(manifest["schema"], 1)
        self.assertEqual([item["filename"] for item in manifest["assets"]], sorted((self.key_name, self.meta_name, self.spv_name)))
        self.assertEqual(manifest["profile"]["sha256"], hashlib.sha256(bytes(range(64))).hexdigest())
        for item in manifest["assets"]:
            self.assertEqual((first / item["filename"]).read_bytes(), (self.source / item["filename"]).read_bytes())
            self.assertEqual(item["sha256"], hashlib.sha256((first / item["filename"]).read_bytes()).hexdigest())
        self.assertFalse((first / "CUSA00004.vkcache").exists())
        self.assertEqual(json.loads((first / "manifest.json").read_text()), manifest)

    def test_incomplete_compute_recipe_rejected_before_output_creation(self) -> None:
        (self.source / self.meta_name).unlink()
        output = self.root / "incomplete"
        with self.assertRaises(InvalidLibrary):
            prepare(self.source, output)
        self.assertFalse(output.exists())

    def test_current_and_legacy_metadata_versions_match_runtime_loader(self) -> None:
        meta = self.source / self.meta_name
        for version in (6, 7):
            meta.write_bytes(struct.pack("<IIQQ", version, 8, self.variant, 0) + b"metadata")
            output = self.root / f"metadata-v{version}"
            manifest = prepare(self.source, output)
            self.assertEqual(len(manifest["assets"]), 3)
        meta.write_bytes(struct.pack("<IIQQ", 8, 8, self.variant, 0) + b"metadata")
        with self.assertRaisesRegex(InvalidLibrary, "incompatible or unpaired"):
            prepare(self.source, self.root / "metadata-v8")

    def test_incomplete_can_be_explicitly_skipped(self) -> None:
        (self.source / self.key_name).unlink()
        missing_hash = 0xBEEF
        missing_name = f"c_0x{missing_hash:016x}.key"
        (self.source / missing_name).write_bytes(struct.pack("<IIQ", 3, 1, missing_hash))
        output = self.root / "filtered"
        manifest = prepare(self.source, output, skip_incomplete=True)
        self.assertEqual(manifest["rejected"], [{"filename": missing_name, "reason": "missing shader variant"}])
        self.assertFalse((output / missing_name).exists())
        self.assertTrue((output / self.spv_name).exists())

    def test_graphics_key_requires_each_referenced_variant(self) -> None:
        graphic = bytearray(1592)
        struct.pack_into("<IIQ", graphic, 0, 3, 0, 0xBEEF)
        (self.source / "g_0x0000000000000001.key").write_bytes(graphic)
        with self.assertRaisesRegex(InvalidLibrary, "graphics key has no shader variant"):
            prepare(self.source, self.root / "graphic-incomplete")
        manifest = prepare(self.source, self.root / "graphic-filtered", skip_incomplete=True)
        self.assertEqual(manifest["rejected"][0]["filename"], "g_0x0000000000000001.key")

    def test_invalid_filename_rejected(self) -> None:
        (self.source / "c_0x0000000000000000_extra.key").write_bytes(b"bad")
        with self.assertRaisesRegex(InvalidLibrary, "filename"):
            prepare(self.source, self.root / "invalid-name")

    def test_output_inside_source_rejected(self) -> None:
        with self.assertRaisesRegex(InvalidLibrary, "separate"):
            prepare(self.source, self.source / "library")

    def test_source_symlink_rejected(self) -> None:
        link = self.root / "source-link"
        try:
            link.symlink_to(self.source, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("symlinks unavailable")
        with self.assertRaisesRegex(InvalidLibrary, "symlink"):
            prepare(link, self.root / "symlink")

    def test_changed_source_changes_digest_without_modifying_first_bundle(self) -> None:
        first = self.root / "before"
        first_manifest = prepare(self.source, first)
        changed = struct.pack("<5I", 0x07230203, 0x10500, 0, 1, 0)
        (self.source / self.spv_name).write_bytes(changed)
        second_manifest = prepare(self.source, self.root / "after")
        first_digest = next(a["sha256"] for a in first_manifest["assets"] if a["filename"] == self.spv_name)
        second_digest = next(a["sha256"] for a in second_manifest["assets"] if a["filename"] == self.spv_name)
        self.assertNotEqual(first_digest, second_digest)
        self.assertNotEqual((first / self.spv_name).read_bytes(), changed)
        with self.assertRaisesRegex(InvalidLibrary, "already exists"):
            prepare(self.source, first)

    def test_recover_compute_only_for_single_glcompute_entrypoint(self) -> None:
        (self.source / self.key_name).unlink()
        header = struct.pack("<5I", 0x07230203, 0x10000, 0, 2, 0)
        (self.source / self.spv_name).write_bytes(header + entrypoint(5))
        output = self.root / "recovered"
        manifest = prepare(self.source, output, recover_compute=True)
        self.assertEqual(manifest["recovered_compute"], [{
            "filename": self.key_name, "meta": self.meta_name, "spv": self.spv_name,
        }])
        self.assertEqual((output / self.key_name).read_bytes(), struct.pack("<IIQ", 3, 1, self.variant))
        self.assertFalse((self.source / self.key_name).exists())

        (self.source / self.spv_name).write_bytes(header + entrypoint(4))
        noncompute = prepare(self.source, self.root / "noncompute", recover_compute=True)
        self.assertEqual(noncompute["recovered_compute"], [])
        (self.source / self.spv_name).write_bytes(header + entrypoint(5) + entrypoint(4))
        mixed = prepare(self.source, self.root / "mixed", recover_compute=True)
        self.assertEqual(mixed["recovered_compute"], [])

    def test_recover_compute_rejects_bad_instruction_bounds(self) -> None:
        (self.source / self.key_name).unlink()
        header = struct.pack("<5I", 0x07230203, 0x10000, 0, 2, 0)
        (self.source / self.spv_name).write_bytes(header + struct.pack("<I", (100 << 16) | 15))
        output = self.root / "bad-instructions"
        with self.assertRaisesRegex(InvalidLibrary, "bounds"):
            prepare(self.source, output, recover_compute=True)
        self.assertFalse(output.exists())

    def test_packed_roundtrip_and_content_hashes(self) -> None:
        output = self.root / "packed"
        manifest = prepare(self.source, output, packed=True)
        pack = (output / "recipes.bin").read_bytes()
        self.assertEqual(manifest["pack"], {
            "filename": "recipes.bin", "size": len(pack), "sha256": hashlib.sha256(pack).hexdigest(),
        })
        expected_offset = 0
        for item in manifest["assets"]:
            self.assertEqual(item["offset"], expected_offset)
            data = pack[item["offset"]:item["offset"] + item["size"]]
            self.assertEqual(data, (self.source / item["filename"]).read_bytes())
            self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
            self.assertFalse((output / item["filename"]).exists())
            expected_offset += item["size"]
        self.assertEqual(expected_offset, len(pack))
        self.assertEqual((output / "profile.bin").read_bytes(), self.profile_bytes())

    def test_packed_recovered_compute_key_is_in_pack(self) -> None:
        (self.source / self.key_name).unlink()
        header = struct.pack("<5I", 0x07230203, 0x10000, 0, 2, 0)
        (self.source / self.spv_name).write_bytes(header + entrypoint(5))
        output = self.root / "packed-recovered"
        manifest = prepare(self.source, output, recover_compute=True, packed=True)
        item = next(a for a in manifest["assets"] if a["filename"] == self.key_name)
        pack = (output / "recipes.bin").read_bytes()
        self.assertEqual(pack[item["offset"]:item["offset"] + item["size"]],
                         struct.pack("<IIQ", 3, 1, self.variant))
        self.assertEqual(len(manifest["recovered_compute"]), 1)
        self.assertFalse((output / self.key_name).exists())

    @staticmethod
    def profile_bytes() -> bytes:
        return bytes(range(64))


if __name__ == "__main__":
    unittest.main()
