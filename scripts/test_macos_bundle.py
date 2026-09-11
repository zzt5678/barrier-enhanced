#!/usr/bin/env python3
"""Exercise the existing bundle script without invoking macOS deployment tools."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "dist/macos/bundle/build_dist.sh.in"


class BundleScriptTests(unittest.TestCase):
    def run_bundle(self, valid_deployer=True):
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
        deployer.write_text("#!/bin/sh\nexit 0\n")
        deployer.chmod(0o755)
        script = SCRIPT.read_text().replace("@CMAKE_RUNTIME_OUTPUT_DIRECTORY@", str(binaries))
        script = script.replace("@CMAKE_BUILD_TYPE@", "Debug").replace("@BARRIER_VERSION@", "test")
        script_path = root / "build_dist.sh"
        script_path.write_text(script)
        environment = dict(os.environ, TERM="dumb",
                           DEPLOYQT=str(deployer if valid_deployer else root / "missing"))
        result = subprocess.run(["bash", str(script_path)], cwd=root, env=environment,
                                text=True, capture_output=True)
        return root, result

    def test_only_shipping_components_are_bundled(self):
        root, result = self.run_bundle()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual({"weave", "weavec", "weaves"},
                         {p.name for p in (root / "Weave.app/Contents/MacOS").iterdir()})

    def test_invalid_explicit_deployer_reports_actionable_error(self):
        _, result = self.run_bundle(False)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("macdeployqt not found; set DEPLOYQT", result.stdout)


if __name__ == "__main__":
    unittest.main()
