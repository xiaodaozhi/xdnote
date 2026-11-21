"""Verify that assembly preserves exact asset and receipt identity."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import assemble_release_candidate as assembly


class ReleaseAssemblyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / 'release-verified'
        self.identity = {'schemaVersion': 1, 'product': 'xdnote', 'version': '0.2.0-rc.1', 'build': 12,
                         'revision': 'a' * 40, 'dirty': False, 'channel': 'rc', 'configuration': 'Release'}
        self.directories = {platform: self.root / platform for platform in assembly.PLATFORMS}
        self.receipts = {platform: {'identity': self.identity, 'assets': {}} for platform in self.directories}
        for directory in self.directories.values():
            directory.mkdir()
        for platform, _, _, suffix, _ in assembly.TARGETS:
            name = f'xdnote-{self.identity["version"]}-{suffix}'
            data = ('test fixture ' + suffix).encode()
            (self.directories[platform] / name).write_bytes(data)
            self.receipts[platform]['assets'][name] = {
                'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'verified': True,
            }
        for platform, receipt in self.receipts.items():
            self.write_receipt(platform, receipt)

    def write_receipt(self, platform, receipt):
        (self.directories[platform] / 'verification.json').write_text(json.dumps(receipt), encoding='utf-8')

    def test_assembles_preflight_inputs_and_preserves_existing_output(self):
        report = assembly.assemble(self.identity, self.directories, self.output)
        self.assertEqual(report, json.loads((self.output / 'preflight.json').read_bytes()))
        self.assertEqual(json.loads((self.output / 'identity.json').read_bytes()), self.identity)
        verification = json.loads((self.output / 'verification.json').read_bytes())
        self.assertEqual(verification['platformReceiptSha256'], {
            platform: hashlib.sha256((directory / 'verification.json').read_bytes()).hexdigest()
            for platform, directory in self.directories.items()
        })
        for platform, receipt in self.receipts.items():
            for name, evidence in receipt['assets'].items():
                self.assertEqual(verification['assets'][name], evidence)
                self.assertEqual((self.output / 'assets' / name).read_bytes(),
                                 (self.directories[platform] / name).read_bytes())
        self.assertEqual(len(report['candidate']['assets']), 6)
        self.assertFalse(report['authenticated'])
        self.assertFalse(report['releaseReady'])
        with self.assertRaisesRegex(ValueError, 'new output directory'):
            assembly.assemble(self.identity, self.directories, self.output)

    def test_rejects_mixed_identity_or_wrong_package_set(self):
        macos = self.receipts['macos']
        for change in ({'revision': 'b' * 40}, {'version': '0.2.0-rc.2'}, {'build': 13}):
            with self.subTest(change=change):
                self.write_receipt('macos', macos | {'identity': self.identity | change})
                with self.assertRaisesRegex(ValueError, 'macos: receipt identity differs'):
                    assembly.assemble(self.identity, self.directories, self.output)
                self.assertFalse(self.output.exists())
        self.write_receipt('macos', macos | {'identity': self.identity | {'build': 12.0}})
        with self.assertRaisesRegex(ValueError, 'clean explicit release identity'):
            assembly.assemble(self.identity, self.directories, self.output)
        self.write_receipt('macos', macos | {'assets': self.receipts['windows']['assets']})
        with self.assertRaisesRegex(ValueError, 'exactly its two candidate packages'):
            assembly.assemble(self.identity, self.directories, self.output)
        self.assertFalse(self.output.exists())

    def test_rejects_missing_symlinked_or_changed_bytes_without_leaving_output(self):
        name = next(iter(self.receipts['linux']['assets']))
        package = self.directories['linux'] / name
        original = package.read_bytes()
        package.unlink()
        for replacement in ('missing', 'symlink', 'changed'):
            with self.subTest(replacement=replacement):
                if replacement == 'symlink':
                    # Creating a symlink may require administrator privileges on Windows.
                    target = self.root / 'original'
                    target.write_bytes(original)
                    try:
                        package.symlink_to(target)
                    except OSError:
                        continue
                elif replacement == 'changed':
                    package.write_bytes(b'x' * len(original))
                with self.assertRaisesRegex(ValueError, 'Missing or invalid asset|Asset verification missing or stale'):
                    assembly.assemble(self.identity, self.directories, self.output)
                self.assertFalse(self.output.exists())
                self.assertFalse(list(self.root.glob('.candidate-*')))
                package.unlink(missing_ok=True)
