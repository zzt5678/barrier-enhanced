import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import weave_release_manifest as release

COMMIT = "12345678" + "a" * 32
IDENTITY = {"build_id": "2.4.0-release+12345678.20260911", "protocol": "1.12"}


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def populate(self, system="Linux"):
        header = bytearray(128)
        if system == "Windows":
            header[:2] = b"MZ"
            header[60:64] = struct.pack("<I", 64)
            header[64:70] = b"PE\0\0" + struct.pack("<H", 0x8664)
        elif system == "Darwin":
            header[:8] = b"\xcf\xfa\xed\xfe" + struct.pack("<I", 0x100000c)
        else:
            header[:6] = b"\x7fELF\x02\x01"
            header[18:20] = struct.pack("<H", 62)
        for name in release.required_files(system):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(header)

    def check_manifest(self, system="Linux", identity=None):
        with patch.object(release, "inspect_binary", return_value=identity or IDENTITY):
            return release.manifest(self.root, COMMIT, system)

    def test_matching_inventory_hashes_exact_files(self):
        self.populate()
        result = self.check_manifest()
        self.assertTrue(result["ok"], result["errors"])
        self.assertEqual(3, len(result["files"]))
        for record in result["files"]:
            self.assertEqual(hashlib.sha256((self.root / record["file"]).read_bytes()).hexdigest(), record["sha256"])
            self.assertEqual("x86_64", record["architecture"])

    def test_missing_gui_fails(self):
        self.populate()
        (self.root / "weave").unlink()
        self.assertIn("RequiredFileMissing:weave", self.check_manifest()["errors"])

    def test_native_thin_macos_inventory(self):
        self.populate("Darwin")
        result = self.check_manifest("Darwin")
        self.assertTrue(result["ok"], result["errors"])
        self.assertEqual({"arm64"}, {r["architecture"] for r in result["files"]})

    def test_mismatched_revision_fails_even_if_all_components_agree(self):
        self.populate()
        result = self.check_manifest(identity=dict(IDENTITY, build_id="2.4.0-release+87654321.20260911"))
        self.assertIn("SourceRevisionMismatch:weave", result["errors"])

    def test_mixed_components_and_legacy_protocol_fail(self):
        self.populate()
        with patch.object(release, "inspect_binary", side_effect=[IDENTITY, dict(IDENTITY, protocol="1.6"), dict(IDENTITY, build_id="other")]):
            result = release.manifest(self.root, COMMIT, "Linux")
        self.assertIn("ProtocolMismatch", result["errors"])
        self.assertIn("ComponentBuildMismatch", result["errors"])

    def test_windows_stage_requires_daemon_and_runtime(self):
        self.populate("Windows")
        result = self.check_manifest("Windows")
        self.assertTrue(result["ok"], result["errors"])
        self.assertIn("statically", result["windows_hook"])
        (self.root / "weaved.exe").unlink()
        (self.root / "platforms/qwindows.dll").unlink()
        errors = self.check_manifest("Windows")["errors"]
        self.assertIn("RequiredFileMissing:weaved.exe", errors)
        self.assertIn("RequiredFileMissing:platforms/qwindows.dll", errors)

    def test_symlink_escape_is_not_probed(self):
        self.populate()
        (self.root / "weave").unlink()
        (self.root / "weave").symlink_to("/bin/sh")
        with patch.object(release, "inspect_binary", return_value=IDENTITY) as probe:
            result = release.manifest(self.root, COMMIT, "Linux")
        self.assertIn("PathEscapesCandidate:weave", result["errors"])
        self.assertEqual(2, probe.call_count)

    def test_changed_binary_fails(self):
        self.populate()
        with patch.object(release, "sha256", side_effect=["a", "b"] * 3), patch.object(release, "inspect_binary", return_value=IDENTITY):
            result = release.manifest(self.root, COMMIT, "Linux")
        self.assertIn("FileChangedDuringInspection:weave", result["errors"])

    def test_invalid_commit_rejected_before_read(self):
        with self.assertRaisesRegex(ValueError, "Full40Hex"):
            release.manifest(self.root, "HEAD", "Linux")

    def test_wrong_platform_and_unknown_architecture_fail(self):
        self.populate()
        (self.root / "weave").write_bytes(b"not a binary")
        result = self.check_manifest()
        self.assertIn("BinaryPlatformMismatch:weave", result["errors"])
        self.assertIn("ArchitectureUnverified:weave", result["errors"])


if __name__ == "__main__":
    unittest.main()
