"""Check the inventory detects stale dependencies and modified license text."""
from copy import deepcopy
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_third_party_licenses import validate_inventory, validate_deployment_notices


class ThirdPartyLicenseInventoryTests(unittest.TestCase):
    def test_qt_notice_version_digest_and_path_must_match(self):
        notices = {"schemaVersion": 1, "qtVersion": "6.8.2", "texts": [{"path": "LICENSES/third-party/Qt.txt", "sha256": hashlib.sha256(b"text").hexdigest()}]}
        dependencies = {"qt": {"version": "6.8.2"}}
        self.assertEqual(validate_deployment_notices(notices, dependencies, lambda _: b"text"), [])
        self.assertTrue(validate_deployment_notices(notices, {"qt": {"version": "changed"}}, lambda _: b"text"))
        self.assertTrue(validate_deployment_notices(notices, dependencies, lambda _: b"changed"))
        notices["texts"][0]["path"] = "../outside"
        self.assertTrue(validate_deployment_notices(notices, dependencies, lambda _: b"text"))
    def setUp(self):
        self.text = b"license text"
        digest = hashlib.sha256(self.text).hexdigest()
        self.lock = {"package": [{"name": "example", "version": "1", "source": "registry+test", "checksum": "abc"}]}
        self.manifest = {"schemaVersion": 1, "rust": [{"name": "example", "version": "1", "archiveSha256": "abc", "upstreamLicense": "MIT OR Apache-2.0", "selectedLicense": "MIT", "licenseFile": "LICENSES/third-party/example.txt", "licenseSha256": digest}], "scintilla": {"version": "5", "sourceSha256": "source", "licenseFile": "LICENSES/third-party/scintilla.txt", "licenseSha256": digest}}
        self.dependencies = {"scintilla": {"version": "5", "sha256": "source"}}

    def check(self, lock=None, manifest=None, dependencies=None, text=None):
        return validate_inventory(lock or self.lock, manifest or self.manifest, dependencies or self.dependencies,
                                  lambda _: self.text if text is None else text)

    def test_complete_matching_inventory(self):
        self.assertEqual(self.check(), [])

    def test_new_or_changed_locked_package_is_reported(self):
        lock = deepcopy(self.lock)
        lock["package"][0]["checksum"] = "changed"
        lock["package"].append({"name": "new", "version": "2", "source": "registry+test", "checksum": "def"})
        problems = self.check(lock=lock)
        self.assertTrue(any("digest mismatch" in problem for problem in problems))
        self.assertTrue(any("no license record" in problem for problem in problems))

    def test_changed_text_and_escape_path_are_rejected(self):
        manifest = deepcopy(self.manifest)
        manifest["rust"][0]["licenseFile"] = "LICENSES/third-party/../../outside.txt"
        problems = self.check(manifest=manifest, text=b"modified")
        self.assertTrue(any("Invalid license path" in problem for problem in problems))
        self.assertTrue(any("digest mismatch" in problem for problem in problems))

    def test_missing_or_changed_scintilla_and_license_choice_are_reported(self):
        manifest = deepcopy(self.manifest)
        manifest["rust"][0]["selectedLicense"] = "GPL"
        manifest["scintilla"]["version"] = "6"
        problems = self.check(manifest=manifest)
        self.assertTrue(any("Selected license" in problem for problem in problems))
        self.assertTrue(any("Scintilla source version" in problem for problem in problems))


if __name__ == "__main__":
    unittest.main()
