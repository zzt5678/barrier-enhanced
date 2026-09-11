#!/usr/bin/env python3
"""Exercise the existing bundle script without invoking macOS deployment tools."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "dist/macos/bundle/build_dist.sh.in"


class BundleScriptTests(unittest.TestCase):
    def run_bundle(self, valid_deployer=True, signing_fails=False,
                   verification_fails=False, image_fails=False, no_minimum=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        binaries = root / "bin"
        binaries.mkdir()
        for name in ("weave", "weavec", "weaves", "unittests", "guiunittests"):
            executable = binaries / name
            executable.write_text("#!/bin/sh\nexit 0\n")
            executable.chmod(0o755)
        (root / "Weave.app/Contents/MacOS").mkdir(parents=True)
        (root / "Weave.app/Contents/MacOS/stale-test-executable").write_text("old build")
        deployer = root / "macdeployqt"
        deployer.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > deploy-args.txt\nexit 0\n")
        deployer.chmod(0o755)
        signer = root / "codesign"
        signer.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" >> sign-args.txt\nexit " +
                          ("1" if signing_fails else "0") + "\n")
        if verification_fails:
            signer.write_text('#!/bin/sh\n[ "$1" != "--verify" ]\n')
        signer.chmod(0o755)
        imager = root / "hdiutil"
        imager.write_text("#!/bin/sh\nexit " + ("1" if image_fails else "0") + "\n")
        imager.chmod(0o755)
        script = SCRIPT.read_text().replace("@CMAKE_RUNTIME_OUTPUT_DIRECTORY@", str(binaries))
        script = script.replace("@CMAKE_BUILD_TYPE@", "Release" if image_fails else "Debug")
        script = script.replace("@BARRIER_VERSION@", "test")
        script_path = root / "build_dist.sh"
        script_path.write_text(script)
        (root / "minimum_macos.sh").write_text((SCRIPT.parent / "minimum_macos.sh").read_text())
        for name, body in {
            "file": "echo Mach-O",
            "otool": "printf 'cmd LC_BUILD_VERSION\\n minos 12.0\\ncmd LC_VERSION_MIN_MACOSX\\n version 14.3\\n'",
            "PlistBuddy": "printf '%s\\n' \"$@\" > plist-args.txt",
        }.items():
            tool = root / name
            tool.write_text("#!/bin/sh\n" + body + "\n")
            tool.chmod(0o755)
        if no_minimum:
            (root / "otool").write_text("#!/bin/sh\nexit 0\n")
        environment = dict(os.environ, TERM="dumb",
                           PATH=str(root) + os.pathsep + os.environ["PATH"],
                           PLISTBUDDY=str(root / "PlistBuddy"),
                           DEPLOYQT=str(deployer if valid_deployer else root / "missing"))
        result = subprocess.run(["bash", str(script_path)], cwd=root, env=environment,
                                text=True, capture_output=True)
        return root, result

    def test_only_shipping_components_are_bundled(self):
        root, result = self.run_bundle()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual({"weave", "weavec", "weaves"},
                         {p.name for p in (root / "Weave.app/Contents/MacOS").iterdir()})
        self.assertIn("--sign", (root / "sign-args.txt").read_text().splitlines())
        self.assertIn("--strict", (root / "sign-args.txt").read_text().splitlines())
        self.assertIn("Set :LSMinimumSystemVersion 14.3",
                      (root / "plist-args.txt").read_text())

    def test_signing_failure_fails_packaging(self):
        _, result = self.run_bundle(signing_fails=True)
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn("Bundle created successfully", result.stdout)

    def test_verification_failure_fails_packaging(self):
        _, result = self.run_bundle(verification_fails=True)
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn("Bundle created successfully", result.stdout)

    def test_disk_image_failure_fails_packaging(self):
        _, result = self.run_bundle(image_fails=True)
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn("Created Weave-test.dmg", result.stdout)

    def test_missing_minimum_os_fails_packaging(self):
        _, result = self.run_bundle(no_minimum=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Could not determine minimum macOS version", result.stdout)

    def test_invalid_explicit_deployer_reports_actionable_error(self):
        _, result = self.run_bundle(False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("macdeployqt not found; set DEPLOYQT", result.stdout)


if __name__ == "__main__":
    unittest.main()
