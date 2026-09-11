import unittest
from unittest.mock import patch
from types import SimpleNamespace
from weave_doctor import assess, inspect_binary


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


if __name__ == "__main__":
    unittest.main()
