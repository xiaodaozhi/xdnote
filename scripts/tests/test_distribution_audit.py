import hashlib
import json
import struct
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import distribution_audit as material
from installer_payload import inventory, product_qt_records


class DistributionAuditTests(unittest.TestCase):
    def test_pre_certificate_comparison_checks_bytes_without_hiding_code_changes(self):
        data = bytearray(1024); data[:2] = b'MZ'; struct.pack_into('<I', data, 0x3c, 128)
        data[128:132] = b'PE\0\0'; struct.pack_into('<H', data, 152, 0x20b)
        expected = hashlib.sha1(data).hexdigest()
        signed = data + b'controlled certificate fixture'
        struct.pack_into('<I', signed, 216, 1234)
        struct.pack_into('<II', signed, 296, len(data), len(signed) - len(data))
        self.assertTrue(material.unsigned_pe_match(signed, expected))
        signed[600] = 1
        self.assertFalse(material.unsigned_pe_match(signed, expected))
        self.assertFalse(material.unsigned_pe_match(b'MZ truncated', expected))
    def test_product_metadata_removes_diagnostics_without_rewriting_module_coverage(self):
        original = [{'sbom': 'base.json', 'matchedBinaryNames': ['Qt6Core.dll', 'Qt6Test.dll', 'qoffscreen.dll'],
                     'packagesWithUnresolvedLicenseFields': ['Test_Attribution'], 'upstreamPackages': 12}]
        result = product_qt_records(original, [{'path': 'Qt6Core.dll'}])
        self.assertEqual(result[0]['matchedBinaryNames'], ['Qt6Core.dll'])
        self.assertEqual(result[0]['packagesWithUnresolvedLicenseFields'], ['Test_Attribution'])
        self.assertEqual(len(original[0]['matchedBinaryNames']), 3)

    def fixture(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name) / 'payload'; qt = Path(tmp.name) / 'sdk'
        (root / 'licenses/qt').mkdir(parents=True); (qt / 'sbom').mkdir(parents=True); (qt / 'bin').mkdir()
        binary = b'controlled Qt file fixture'
        (root / 'Qt6Core.dll').write_bytes(binary); (qt / 'bin/Qt6Core.dll').write_bytes(binary)
        value = {'files': [{'SPDXID': 'file-core', 'fileName': './bin/Qt6Core.dll',
                            'checksums': [{'algorithm': 'SHA1', 'checksumValue': '0' * 40}],
                            'licenseConcluded': 'LGPL-3.0-only', 'copyrightText': 'upstream fixture'}]}
        text = json.dumps(value)
        for path in (root / 'licenses/qt/base.json', qt / 'sbom/base.json'): path.write_text(text, encoding='utf-8')
        records = [{'sbom': 'base.json', 'sha256': material.digest(root / 'licenses/qt/base.json')}]
        return root, qt, records

    def test_actual_sdk_match_and_upstream_checksum_difference_are_separate(self):
        root, qt, records = self.fixture()
        result = material.qt_files(root, qt, records, {item['path']: item for item in inventory(root)})['Qt6Core.dll']
        self.assertTrue(result['sdkSha256Matches'])
        self.assertFalse(result['upstreamSha1Matches'])
        self.assertEqual(result['upstreamFileChecksums']['SHA1'], '0' * 40)

    def test_changed_supplier_binary_is_rejected(self):
        root, qt, records = self.fixture()
        (qt / 'bin/Qt6Core.dll').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'supplier byte mismatch'):
            material.qt_files(root, qt, records, {item['path']: item for item in inventory(root)})

    def test_missing_required_materials_cannot_be_reported_as_complete(self):
        root, qt, records = self.fixture()
        with self.assertRaisesRegex(ValueError, 'Required distribution material missing'):
            material.audit(root, {'ok': True, 'files': inventory(root), 'qtSbom': records}, qt)


if __name__ == '__main__':
    unittest.main()
