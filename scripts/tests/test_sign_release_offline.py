"""Signed attachments bind to one candidate and are verifiable without private keys."""

import base64
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_updates as signing


class ReleaseSigningTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.fields = {signing.KEY_ID: 'fixture', signing.MANIFEST_KEY: '01' * 32, signing.SPARKLE_KEY: '02' * 32}
        self.keys = self.root / 'offline.txt'
        self.keys.write_text(''.join(f'{name}={value}\n' for name, value in self.fields.items()))
        self.identity = dict(product='xdnote', version='0.2.0-rc.1', build=12, revision='a' * 40,
                             dirty=False, channel='rc', configuration='Release')
        self.assets = self.root / 'assets'
        self.assets.mkdir()
        self.verification = {'identity': self.identity, 'assets': {}}
        for _, _, _, suffix, _ in signing.release_candidate.TARGETS:
            name = 'xdnote-0.2.0-rc.1-' + suffix
            path = self.assets / name
            path.write_text('fixture ' + name)
            self.verification['assets'][name] = {'bytes': path.stat().st_size, 'sha256': signing.digest(path), 'verified': True}
        self.output = self.root / 'signed'
        self.operations = []

    def run_tool(self, tool, operation, *arguments, credentials=None):
        self.operations.append(operation)
        expected = {'check-signing-keys': self.fields, 'sign-archive': {signing.SPARKLE_KEY: self.fields[signing.SPARKLE_KEY]},
                    'sign-appcast': {signing.SPARKLE_KEY: self.fields[signing.SPARKLE_KEY]},
                    'sign': {name: self.fields[name] for name in (signing.KEY_ID, signing.MANIFEST_KEY)}}
        self.assertEqual(credentials, expected.get(operation))
        if operation == 'sign-archive':
            archive, version, build, output = arguments
            output.write_text(json.dumps({'schema_version': 1, 'version': version, 'build': int(build),
                                          'filename': archive.name, 'bytes': archive.stat().st_size,
                                          'sha256': signing.digest(archive), 'ed_signature': base64.b64encode(b's' * 64).decode()}))
        elif operation == 'sign-appcast':
            arguments[1].write_bytes(arguments[0].read_bytes() + b'<!-- sparkle-signatures:\nfixture\n-->\n')
        elif operation == 'sign':
            arguments[1].write_text('{}')

    def test_signed_contents_and_public_reverification(self):
        with patch.object(signing, 'run_tool', side_effect=self.run_tool):
            signing.sign_release(self.identity, self.assets, self.verification, self.keys, self.output, tool='tool')
            manifest = json.loads((self.output / 'release.json').read_bytes())
            self.assertEqual(manifest, signing.release_candidate.manifest(self.identity, self.assets, self.verification))
            archive = signing.sparkle_archive(manifest)
            item = ET.parse(self.output / 'appcast.xml').find('channel/item')
            self.assertEqual(item.findtext(f'{{{signing.SPARKLE_NS}}}version'), '12')
            self.assertEqual(item.findtext(f'{{{signing.SPARKLE_NS}}}channel'), 'rc')
            self.assertEqual(item.find('enclosure').get('url'), archive['url'])
            self.assertEqual(item.find('enclosure').get('length'), str(archive['bytes']))
            self.assertEqual(set(path.name for path in self.output.iterdir()), {*signing.ATTACHMENTS, 'SHA256SUMS'})
            self.assertIn(f"{archive['sha256']}  {archive['filename']}", (self.output / 'SHA256SUMS').read_text())
            self.assertEqual(self.operations[:4], ['check-signing-keys', 'sign-archive', 'sign-appcast', 'sign'])
            self.operations.clear()
            signing.verify_release(self.identity, self.assets, self.verification, self.output, 'tool')
            self.assertEqual(self.operations, ['verify-archive', 'verify-appcast', 'verify'])
            (self.output / 'SHA256SUMS').write_text('wrong')
            with self.assertRaisesRegex(ValueError, 'SHA256SUMS differs'):
                signing.verify_release(self.identity, self.assets, self.verification, self.output, 'tool')
            with self.assertRaisesRegex(ValueError, 'existing output'):
                signing.sign_release(self.identity, self.assets, self.verification, self.keys, self.output, tool='tool')

    def test_failed_verification_leaves_no_output(self):
        def fail_verify(tool, operation, *arguments, credentials=None):
            if operation == 'verify-appcast':
                raise ValueError('Appcast verification failed')
            return self.run_tool(tool, operation, *arguments, credentials=credentials)

        with patch.object(signing, 'run_tool', side_effect=fail_verify):
            with self.assertRaisesRegex(ValueError, 'Appcast verification failed'):
                signing.sign_release(self.identity, self.assets, self.verification, self.keys, self.output, tool='tool')
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob('.sign-release-*')), [])

    def test_environment_isolation_and_diagnostic_redaction(self):
        with patch.dict(signing.os.environ, self.fields), patch.object(signing.subprocess, 'run') as run:
            self.assertEqual(signing.signing_inputs(None), self.fields)
            run.return_value = subprocess.CompletedProcess([], 2, b'', self.fields[signing.SPARKLE_KEY].encode())
            with self.assertRaisesRegex(ValueError, '^Release signing tool: verify-appcast failed$'):
                signing.run_tool('tool', 'verify-appcast', self.output / 'appcast.xml')
            self.assertTrue(set(self.fields).isdisjoint(run.call_args.kwargs['env']))
            run.return_value = subprocess.CompletedProcess([], 0, b'', b'')
            signing.run_tool('tool', 'sign', 'release.json', 'release.json.sig',
                             credentials={name: self.fields[name] for name in (signing.KEY_ID, signing.MANIFEST_KEY)})
            self.assertNotIn(signing.SPARKLE_KEY, run.call_args.kwargs['env'])
            self.assertNotIn(self.fields[signing.MANIFEST_KEY], run.call_args.args[0])
