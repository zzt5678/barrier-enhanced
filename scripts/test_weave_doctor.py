import unittest
from unittest.mock import patch
from types import SimpleNamespace
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import shutil
import sys
from weave_doctor import assess, inspect_binary, binary_architecture, running_components


class IdentityTests(unittest.TestCase):
    def test_gui_and_core_version_formats(self):
        for text in ["Weave 2.4.0+abc\nProtocol version 1.12\n",
                     "weavec 2.4.0\nBuild ID 2.4.0+abc\nProtocol version 1.12\n"]:
            with patch("pathlib.Path.is_file", return_value=True), patch(
                    "subprocess.run", return_value=SimpleNamespace(
                        returncode=0, stdout=text, stderr="")):
                result = inspect_binary("/example/weave")
                self.assertEqual("2.4.0+abc", result["build_id"])
                self.assertEqual("1.12", result["protocol"])

    def test_matching_components(self):
        self.assertEqual([], assess([{"build_id": "a", "protocol": "1.12"}] * 3))

    def test_same_protocol_different_build(self):
        self.assertIn("ComponentBuildMismatch", assess([
            {"build_id": "a", "protocol": "1.12"},
            {"build_id": "b", "protocol": "1.12"}]))

    def test_legacy_or_other_major(self):
        for protocol in ["1.6", "2.12"]:
            self.assertIn("ProtocolMismatch", assess([{"build_id": "a", "protocol": protocol}]))

    def test_missing_component_is_not_success(self):
        self.assertIn("ComponentMissing", assess([{"error": "ComponentMissing"}]))

    def test_timeout_has_distinct_error(self):
        with patch("pathlib.Path.is_file", return_value=True), patch(
                "subprocess.run", side_effect=subprocess.TimeoutExpired("weave", 5)):
            self.assertEqual("VersionTimeout", inspect_binary("/example/weave")["error"])

    def test_real_executable_version(self):
        if os.name == "nt":
            self.skipTest("POSIX fixture")
        with tempfile.TemporaryDirectory() as root:
            executable = Path(root) / "weavec"
            executable.write_text("#!/bin/sh\nprintf 'Build ID test-build\\nProtocol version 1.12\\n'\n")
            executable.chmod(0o700)
            result = inspect_binary(executable)
            self.assertEqual("test-build", result["build_id"])
            executable.write_text("#!/bin/sh\nexit 1\n")
            self.assertEqual("IdentityUnavailable", inspect_binary(executable)["error"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux process image fixture")
    def test_real_running_process_image(self):
        with tempfile.TemporaryDirectory() as root:
            image = Path(root) / "weavec"
            shutil.copy2("/bin/sleep", image)
            process = subprocess.Popen([str(image), "20"])
            try:
                found, errors = running_components()
                self.assertEqual([], errors)
                component = next(item for item in found if item["pid"] == process.pid)
                self.assertEqual(str(image), component["executable_path"])
                self.assertEqual(f"/proc/{process.pid}/exe", component["inspection_path"])
                image.unlink()
                found, _ = running_components()
                component = next(item for item in found if item["pid"] == process.pid)
                self.assertTrue(component["executable_path"].endswith(" (deleted)"))
                self.assertNotEqual("unknown", binary_architecture(component["inspection_path"]))
            finally:
                process.terminate()
                process.wait(timeout=5)

    @unittest.skipIf(os.name == "nt", "POSIX executable timeout fixture")
    def test_real_version_timeout(self):
        with tempfile.TemporaryDirectory() as root:
            executable = Path(root) / "weavec"
            executable.write_text("#!/bin/sh\nexec sleep 20\n")
            executable.chmod(0o700)
            self.assertEqual("VersionTimeout", inspect_binary(executable)["error"])

    def test_header_architecture_not_host_guess(self):
        fixtures = []
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = struct.pack("<H", 183)
        fixtures.append((elf, "arm64"))
        pe = bytearray(70)
        pe[:2] = b"MZ"
        pe[60:64] = struct.pack("<I", 64)
        pe[64:] = b"PE\0\0" + struct.pack("<H", 0x8664)
        fixtures.append((pe, "x86_64"))
        fixtures.append((b"\xcf\xfa\xed\xfe" + struct.pack("<I", 0x100000c), "arm64"))
        fixtures.append((b"#!/bin/sh", "unknown"))
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "image"
            for contents, expected in fixtures:
                path.write_bytes(contents)
                self.assertEqual(expected, binary_architecture(path))

    def test_linux_process_scan_uses_image_link_and_exact_names(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            for pid, name in [(12, "weavec"), (13, "weavec-helper"), (14, "weaves")]:
                entry = root / str(pid)
                entry.mkdir()
                (entry / "comm").write_text(name + "\n")
                if pid != 14:
                    (entry / "exe").symlink_to("/old/weavec (deleted)")
                (entry / "cmdline").write_text("SECRET MUST NOT BE READ")
            found, errors = running_components("Linux", root)
            self.assertEqual([], errors)
            self.assertEqual([12, 14], [item["pid"] for item in found])
            self.assertEqual(str(root / "12/exe"), found[0]["inspection_path"])
            self.assertEqual("/old/weavec (deleted)", found[0]["executable_path"])
            self.assertEqual("RunningImageUnavailable", found[1]["error"])
            self.assertNotIn("SECRET", str(found))

    def test_platform_process_scan_timeout_is_error(self):
        with patch("subprocess.run", side_effect=subprocess.TimeoutExpired("ps", 5)):
            self.assertEqual(([], ["RunningProcessScanUnavailable"]), running_components("Darwin"))

    def test_mac_disk_path_limitation_is_explicit(self):
        with patch("subprocess.run", return_value=SimpleNamespace(stdout=
                   "12 /Applications/Weave.app/Contents/MacOS/weavec\n13 /tmp/weavec-other\n")):
            found, errors = running_components("Darwin")
            self.assertEqual([], errors)
            self.assertEqual(1, len(found))
            self.assertEqual("process-disk-path", found[0]["identity_source"])


if __name__ == "__main__":
    unittest.main()
