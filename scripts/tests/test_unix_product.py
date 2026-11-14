"""Product boundary and supplier-code evidence, without native execution."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import product_deployment_unix as product
from unix_binary_evidence import code_sections


def elf():
    data = bytearray(129); data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', data, 40, 64); struct.pack_into('<HH', data, 58, 64, 1)
    struct.pack_into('<IIQQQQIIQQ', data, 64, 0, 1, 4, 0, 128, 1, 0, 0, 1, 0)
    data[-1] = 0xc3
    return data


def macho():
    data = bytearray(185)
    struct.pack_into('<IIIIIIII', data, 0, 0xfeedfacf, 0x1000007, 3, 6, 1, 152, 0, 0)
    struct.pack_into('<II', data, 32, 0x19, 152); struct.pack_into('<I', data, 96, 1)
    struct.pack_into('<16s16sQQIIIIIIII', data, 104, b'__text', b'__TEXT', 0, 1, 184, 0, 0, 0, 0x80000400, 0, 0, 0)
    data[-1] = 0xc3
    return data


class UnixProductTests(unittest.TestCase):
    def test_pinned_native_runtime_license_rejects_modified_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / 'tools/notices').mkdir(parents=True)
            source = root / 'tools/notices/icu.txt'; source.write_bytes(b'original license')
            config = {'qtVersion':product.desktop.DEPS['qt']['version'], 'runtime':[{'licenseFile':'tools/notices/icu.txt','sha256':product.digest(source)}]}
            (root / 'tools/unix-runtime-notices.json').write_text(json.dumps(config))
            product.runtime_notices(root); source.write_bytes(b'changed license')
            with self.assertRaisesRegex(ValueError, 'license changed'): product.runtime_notices(root)

    def test_rejects_diagnostic_and_quick_artifacts_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('lib/libQt6Quick.so.6', 'xdnote.app/Contents/Frameworks/QtQml.framework/QtQml',
                         'plugins/platforms/libqoffscreen.so', 'bin/xdnote_desktop_tests', 'qml/QtQuick/qmldir', 'lib/libQt6Test.so.6'):
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.touch()
                with self.subTest(name=name), self.assertRaises(ValueError): product.require_product(root)
                path.unlink()
            (root / 'bin/xdnote').touch(); product.require_product(root)

    def test_executable_sections_survive_metadata_changes_but_detect_code_changes(self):
        for factory in (elf, macho):
            with self.subTest(format=factory.__name__):
                data = factory(); original = code_sections(data)
                changed_metadata = bytearray(data); changed_metadata[7 if factory is elf else 24] ^= 1
                self.assertEqual(code_sections(changed_metadata), original)
                data[-1] ^= 1; self.assertNotEqual(code_sections(data), original)
                with self.assertRaises(ValueError): code_sections(data[:-1])

    def test_fat_macho_preserves_all_architecture_code_evidence(self):
        thin = macho(); fat = struct.pack('>IIIIIII', 0xcafebabe, 1, 0x1000007, 3, 28, len(thin), 0) + thin
        self.assertEqual(len(code_sections(fat)), 1)
        with self.assertRaises(ValueError): code_sections(fat[:-1])

    def test_data_only_elf_compares_payload_and_detects_data_changes(self):
        data = elf(); struct.pack_into('<Q', data, 72, 2)
        original = code_sections(data)
        self.assertTrue(next(iter(original)).startswith('elf-readonly-data:'))
        data[-1] ^= 1; self.assertNotEqual(code_sections(data), original)
        with self.assertRaises(ValueError): code_sections(data[:-1])

    def test_supplier_code_change_and_unclassified_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); stage, qt = root / 'stage', root / 'qt'
            for folder in (stage / 'lib', stage / 'licenses/qt', qt / 'lib', qt / 'sbom'): folder.mkdir(parents=True)
            original = elf(); (qt / 'lib/libQt6Core.so.6.8.2').write_bytes(original)
            deployed = bytearray(original); deployed[7] = 1
            (stage / 'lib/libQt6Core.so.6.8.2').write_bytes(deployed)
            raw = json.dumps({'files':[{'fileName':'./lib/libQt6Core.so.6.8.2', 'SPDXID':'qt-core'}]}).encode()
            for folder in (qt / 'sbom', stage / 'licenses/qt'): (folder / 'base.spdx.json').write_bytes(raw)
            records = [{'sbom':'base.spdx.json','sha256':product.digest(qt / 'sbom/base.spdx.json')}]
            evidence = product.material_evidence(stage, qt, records, 'Linux')
            core = next(f for f in evidence['files'] if f['path'].startswith('lib/'))
            self.assertFalse(core['evidence']['byteIdenticalToSdk']); self.assertTrue(core['evidence']['nativeSectionsMatchSdk'])
            deployed[-1] ^= 1; (stage / 'lib/libQt6Core.so.6.8.2').write_bytes(deployed)
            with self.assertRaisesRegex(ValueError, 'executable sections differ'): product.material_evidence(stage, qt, records, 'Linux')
            (stage / 'lib/libQt6Core.so.6.8.2').write_bytes(original); (stage / 'unexpected.bin').touch()
            with self.assertRaisesRegex(ValueError, 'Unclassified'): product.material_evidence(stage, qt, records, 'Linux')


if __name__ == '__main__': unittest.main()
