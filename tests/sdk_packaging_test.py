"""Fast SDK distribution guards; no compiler, network, or Skia required."""

import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sdk


class DistributionGuards(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tabengine-packaging-")
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name)
        self.source = {"source_commit": "a" * 40, "source_dirty": False,
                       "source_branch": "main"}
        self.manifest = {
            "version": re.search(r"project\(TabEngine VERSION ([0-9.]+)",
                                 (sdk.ROOT / "CMakeLists.txt").read_text()).group(1),
            "configuration": "Debug", "platform": "windows-x64",
            "provenance": self.source.copy(), "files": {},
        }
        self.license = self.stage / "licenses/nlohmann-json/LICENSE.MIT"
        self.license.parent.mkdir(parents=True)
        self.license.write_bytes((sdk.ROOT / "third_party/json/LICENSE.MIT").read_bytes())
        self.manifest["files"][self.license.relative_to(self.stage).as_posix()] = "hashed-by-verifier"
        self.mcp = self.stage / "tools/mcp"
        package = self.mcp / "node_modules/fixture"
        package.mkdir(parents=True)
        (package / "package.json").write_text(json.dumps({
            "name": "fixture", "version": "1.0.0", "license": "MIT"}))
        (package / "LICENSE").write_text("Fixture license notice")
        (self.mcp / "package-lock.json").write_text(json.dumps({"packages": {
            "": {"name": "fixture-adapter"},
            "node_modules/fixture": {"version": "1.0.0", "license": "MIT"},
        }}))
        self.inventory = self.mcp / "THIRD_PARTY_LICENSES.json"
        self.inventory.write_text(json.dumps(sdk.mcp_dependency_licenses(self.mcp)))
        self.manifest["files"][self.inventory.relative_to(self.stage).as_posix()] = "hashed-by-verifier"
        self.manifest["files"]["tools/mcp/node_modules/fixture/LICENSE"] = "hashed-by-verifier"

    def validate(self):
        sdk.validate_sdk_metadata("Debug", self.stage, self.manifest, self.source)

    def test_complete_distribution_is_accepted(self):
        self.validate()

    def test_missing_truncated_or_unmanifested_json_license_is_rejected(self):
        original = self.license.read_bytes()
        self.license.unlink()
        with self.assertRaisesRegex(SystemExit, "license is missing"):
            self.validate()
        self.license.write_bytes(original[:80])
        with self.assertRaisesRegex(SystemExit, "license text differs"):
            self.validate()
        self.license.write_bytes(original)
        self.manifest["files"].pop(self.license.relative_to(self.stage).as_posix())
        with self.assertRaisesRegex(SystemExit, "not listed"):
            self.validate()

    def test_wrong_configuration_platform_and_version_are_rejected(self):
        for key, value, message in (("configuration", "Release", "configuration"),
                                    ("platform", "linux-x64", "platform"),
                                    ("version", "0.0.0", "version")):
            with self.subTest(key=key):
                previous = self.manifest[key]
                self.manifest[key] = value
                with self.assertRaisesRegex(SystemExit, message):
                    self.validate()
                self.manifest[key] = previous

    def test_dirty_missing_branch_and_stale_publication_sources_are_rejected(self):
        for key, value in (("source_dirty", True), ("source_dirty", None),
                           ("source_branch", "feature"), ("source_commit", "b" * 40)):
            with self.subTest(key=key, value=value):
                self.manifest["provenance"] = {**self.source, key: value}
                with self.assertRaisesRegex(SystemExit, "Publication"):
                    self.validate()

    def test_inventory_tracks_only_lockfile_packages(self):
        benchmark = self.mcp / "node_modules/fixture/benchmark"
        benchmark.mkdir()
        (benchmark / "package.json").write_text(json.dumps({
            "name": "benchmark", "version": "1.0.0"}))
        self.validate()
        self.assertEqual(len(sdk.mcp_dependency_licenses(self.mcp)), 1)

    def test_missing_notice_dependency_and_stale_inventory_are_rejected(self):
        notice = self.mcp / "node_modules/fixture/LICENSE"
        notice.unlink()
        with self.assertRaisesRegex(SystemExit, "license is missing"):
            self.validate()
        notice.write_text("Fixture license notice")
        self.inventory.write_text("[]")
        with self.assertRaisesRegex(SystemExit, "inventory does not match"):
            self.validate()
        (self.mcp / "node_modules/fixture/package.json").unlink()
        with self.assertRaisesRegex(SystemExit, "dependency is missing"):
            sdk.mcp_dependency_licenses(self.mcp)

    def test_empty_or_unmanifested_dependency_notice_is_rejected(self):
        notice = self.mcp / "node_modules/fixture/LICENSE"
        notice.write_text(" ")
        with self.assertRaisesRegex(SystemExit, "missing or empty"):
            self.validate()
        notice.write_text("Fixture license notice")
        self.manifest["files"].pop("tools/mcp/node_modules/fixture/LICENSE")
        with self.assertRaisesRegex(SystemExit, "not listed"):
            self.validate()


if __name__ == "__main__":
    unittest.main()
