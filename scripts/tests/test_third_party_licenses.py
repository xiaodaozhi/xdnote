"""Check the inventory detects stale dependencies and modified license text."""
from copy import deepcopy
import hashlib
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_third_party_licenses import (
    DEPLOYMENT_NOTICE_PATHS, license_choices, source_license_texts, validate_deployment_notices, validate_inventory,
)
import deployment_notices


class InstalledLicenseNoticeTests(unittest.TestCase):
    def test_repository_links_resolve_in_sealed_product_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            licenses = Path(temporary) / 'licenses'
            shutil.copytree(deployment_notices.ROOT / 'LICENSES', licenses / 'project-and-source-dependencies')
            (licenses / 'qt').mkdir()
            replacement = licenses / 'qt/REPLACEMENT.md'
            replacement.write_bytes(b'Test replacement instructions')
            revision = 'a' * 40
            identity = {'buildVersion': {'revision': revision, 'dirty': False}}
            deployment_notices.collect(licenses, identity)
            notice = licenses / deployment_notices.SOURCE.name
            contents = notice.read_text(encoding='utf-8')
            self.assertIn('](qt/REPLACEMENT.md)', contents)
            self.assertIn(f'https://github.com/xiaodaozhi/xdnote/blob/{revision}/docs/engineering/licensing.md', contents)
            self.assertNotIn('](LICENSES/', contents)
            self.assertNotIn('](docs/', contents)
            self.assertNotIn('](tools/', contents)
            for target in deployment_notices.LINK.findall(contents):
                if not target.startswith(('https://', 'http://', '#')):
                    self.assertTrue((licenses / target.split('#')[0]).is_file(), target)
            source_digest = hashlib.sha256(deployment_notices.SOURCE.read_bytes()).hexdigest()
            self.assertEqual(deployment_notices.verify(notice, identity), source_digest)
            replacement.unlink()
            with self.assertRaisesRegex(ValueError, 'material is missing'):
                deployment_notices.verify(notice, identity)
            notice.write_bytes(deployment_notices.SOURCE.read_bytes())
            self.assertEqual(deployment_notices.verify(notice, identity), source_digest)


class ThirdPartyLicenseInventoryTests(unittest.TestCase):
    def test_qt_notice_version_digest_and_path_must_match(self):
        digest = hashlib.sha256(b"text").hexdigest()
        notices = {"schemaVersion": 1, "qtVersion": "6.8.2",
                   "texts": [{"path": path, "sha256": digest} for path in sorted(DEPLOYMENT_NOTICE_PATHS)]}
        dependencies = {"qt": {"version": "6.8.2"}}
        self.assertEqual(validate_deployment_notices(notices, dependencies, lambda _: b"text"), [])
        self.assertTrue(validate_deployment_notices(notices, {"qt": {"version": "changed"}}, lambda _: b"text"))
        self.assertTrue(validate_deployment_notices(notices, dependencies, lambda _: b"changed"))
        missing = deepcopy(notices); missing["texts"].pop()
        self.assertTrue(validate_deployment_notices(missing, dependencies, lambda _: b"text"))
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

    def test_combined_license_keeps_required_unicode_terms(self):
        record = self.manifest['rust'][0]
        record['upstreamLicense'] = '(MIT OR Apache-2.0) AND Unicode-3.0'
        record['selectedLicense'] = 'MIT AND Unicode-3.0'
        record['additionalTexts'] = [{'licenseFile': 'LICENSES/third-party/unicode.txt',
            'licenseSha256': hashlib.sha256(self.text).hexdigest()}]
        self.assertEqual(self.check(), [])
        record['selectedLicense'] = 'MIT'
        self.assertTrue(any('Selected license' in p for p in self.check()))
        record['selectedLicense'] = 'MIT AND Unicode-3.0'
        record['additionalTexts'][0]['licenseSha256'] = 'changed'
        self.assertTrue(any('digest mismatch' in p for p in self.check()))
        record['additionalTexts'][0]['licenseFile'] = '../outside'
        self.assertTrue(any('Invalid license path' in p for p in self.check()))

    def test_payload_materials_include_additional_texts(self):
        self.manifest['rust'][0]['additionalTexts'] = [{'licenseFile': 'LICENSES/third-party/unicode.txt'}]
        self.assertEqual([r['licenseFile'] for r in source_license_texts(self.manifest)], [
            'LICENSES/third-party/example.txt', 'LICENSES/third-party/unicode.txt', 'LICENSES/third-party/scintilla.txt'])

    def test_spdx_choice_syntax_and_legacy_spelling(self):
        self.assertEqual(license_choices('MIT/Apache-2.0'), license_choices('MIT OR Apache-2.0'))
        self.assertNotEqual(license_choices('Apache-2.0 WITH LLVM-exception'), license_choices('Apache-2.0'))
        for expression in ('MIT AND', '(MIT OR Apache-2.0', 'MIT; GPL', 'MIT WITH', 'MIT OR OR BSD', ''):
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                license_choices(expression)

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
