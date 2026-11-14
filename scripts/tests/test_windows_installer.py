"""Installer input guards; native engine behavior has a separate CI fixture."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from installer_payload import inventory, verify_payload
from installer_common import ROOT, build_output

spec = importlib.util.spec_from_file_location('windows_installer', ROOT / 'scripts/build-windows-installer.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class WindowsInstallerTests(unittest.TestCase):
    def test_manifest_rejects_unsafe_and_case_colliding_windows_names(self):
        for name in ('../escape', '/root', 'C:/root', 'a\\..\\b', 'a//b', 'nul.txt', 'COM1/test',
                     'x. /file', 'file:stream', 'install-manifest.txt', 'unins000.exe', 'versions/a', 'a\tfile'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                installer.payload_manifest({'files': [{'path': name, 'sha256': 'a' * 64}]})
        with self.assertRaises(ValueError):
            installer.payload_manifest({'files': [{'path': name, 'sha256': 'a' * 64} for name in ('ABC.dll', 'abc.dll')]})

    def test_manifest_is_bounded_deterministic_and_contains_real_digests(self):
        rows = [{'path': '中文/a.dll', 'sha256': 'b' * 64}, {'path': 'xdnote.exe', 'sha256': 'a' * 64}]
        text = installer.payload_manifest({'files': rows})
        self.assertEqual(text, installer.payload_manifest({'files': list(reversed(rows))}))
        self.assertTrue(text.startswith('xdnote-install-files-v1\n'))
        self.assertIn('b' * 64 + '\t中文\\a.dll\n', text)
        with self.assertRaises(ValueError):
            installer.payload_manifest({'files': [{'path': 'app.exe', 'sha256': 'not-a-digest'}]})
        with self.assertRaises(ValueError):
            installer.payload_manifest({'files': []})

    def test_seal_detects_extra_missing_and_changed_payloads(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            app = root / 'xdnote.exe'; app.write_bytes(b'fixture')
            audit = {'ok': True, 'files': inventory(root)}
            verify_payload(root, audit)
            extra = root / 'extra.dll'; extra.write_bytes(b'unknown')
            with self.assertRaises(ValueError): verify_payload(root, audit)
            extra.unlink(); app.write_bytes(b'changed')
            with self.assertRaises(ValueError): verify_payload(root, audit)
            app.unlink()
            with self.assertRaises(ValueError): verify_payload(root, audit)

    def test_engine_script_keeps_data_and_default_association_boundaries(self):
        source = (ROOT / 'apps/desktop/packaging/windows/installer.iss.in').read_text(encoding='utf-8')
        self.assertIn('PrivilegesRequired=lowest', source)
        self.assertIn('AppMutex=Local\\xdnote.desktop.running', source)
        self.assertIn('CloseApplications=no', source)
        self.assertIn('uninsneveruninstall', source)
        self.assertIn('Flags: unchecked', source)
        self.assertNotIn('UserChoice', source)
        self.assertNotIn('[UninstallDelete]', source)
        self.assertNotIn('{userappdata}', source)
        self.assertNotIn('uninsdeletekey', source)
        self.assertIn('RemoveOwnedRegistration', source)

    def test_only_project_build_outputs_are_allowed(self):
        self.assertEqual(build_output(ROOT / 'build/installer/output.exe'), ROOT / 'build/installer/output.exe')
        for name in (ROOT, ROOT / 'build', ROOT / 'build/../../foreign/output'):
            with self.assertRaises(ValueError): build_output(name)

    def test_pins_and_workflow_are_manual_and_independent_of_desktop_matrix(self):
        config = json.loads((ROOT / 'scripts/installer-toolchain.json').read_text())['inno_setup']
        self.assertEqual(config['version'], '6.7.3')
        self.assertEqual(len(config['sha256']), 64)
        text = (ROOT / '.github/workflows/windows-installer.yml').read_text(encoding='utf-8')
        self.assertIn('workflow_dispatch:', text)
        self.assertNotIn('pull_request:', text)
        self.assertNotIn('push:', text)
        self.assertNotIn('scripts/desktop.py test', text)
        self.assertNotIn('matrix:', text)
        self.assertIn('options: [fixtures, product]', text)
        self.assertIn("if: inputs.scope == 'product'", text)
        self.assertIn('test-windows-product-install.ps1', text)
        self.assertIn('scripts/distribution_audit.py', text)
        self.assertIn('evidence only', text)
        desktop = (ROOT / '.github/workflows/desktop-admission.yml').read_text(encoding='utf-8')
        self.assertIn("if: inputs.scope != 'installer-windows'", desktop)
        self.assertIn("'Windows installer verification'", desktop)


if __name__ == '__main__':
    unittest.main()
