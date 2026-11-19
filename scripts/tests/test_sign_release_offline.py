"""The offline signer binds both keys and the one verified candidate."""

import base64
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sign_release_offline as signing


class OfflineSigningTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.keys = self.root / 'offline.txt'
        self.manifest_seed = b'\x01' * 32
        self.sparkle_seed = b'\x02' * 32
        self.keys.write_text(f'# fixture\nXDNOTE_UPDATE_KEY_ID=fixture\n'
                             f'XDNOTE_UPDATE_SIGNING_KEY_HEX={self.manifest_seed.hex()}\n'
                             f'XDNOTE_SPARKLE_SIGNING_KEY_HEX={self.sparkle_seed.hex()}\n')
        tools = self.root / 'tools'
        tools.mkdir()
        self.manifest_public = b'\x03' * 32
        self.sparkle_public = b'\x04' * 32
        (tools / 'update-trust.json').write_text(json.dumps({'keys': [
            {'key_id': 'fixture', 'public_key': self.manifest_public.hex()},
        ]}))
        (tools / 'sparkle-update-key.json').write_text(json.dumps({
            'publicKey': base64.b64encode(self.sparkle_public).decode('ascii'),
        }))
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

    def checked(self, arguments, operation, *, data):
        self.operations.append(operation)
        if arguments[0] == 'openssl':
            public_key = {self.manifest_seed: self.manifest_public, self.sparkle_seed: self.sparkle_public}
            return signing.PUBLIC_DER + public_key.get(data[len(signing.PRIVATE_DER):], b'')
        self.assertEqual(arguments[:3], ['sign_update', '--ed-key-file', '-'])
        key = base64.b64decode(data)
        using_manifest = any(isinstance(argument, Path) and argument.name == 'release.json' for argument in arguments)
        self.assertEqual(key, self.manifest_seed if using_manifest else self.sparkle_seed)
        return base64.b64encode(b's' * 64) if operation == 'Ed25519 signing' else b''

    def run_signer(self, checked):
        with patch.object(signing, 'ROOT', self.root), patch.object(signing, 'checked', side_effect=checked):
            signing.sign_release(self.identity, self.assets, self.verification, self.keys, self.output,
                                 tool='sign_update', openssl='openssl')

    def test_outputs_contain_one_verified_candidate(self):
        self.run_signer(self.checked)
        manifest = json.loads((self.output / 'release.json').read_bytes())
        self.assertEqual(manifest, signing.release_candidate.manifest(self.identity, self.assets, self.verification))
        archive = next(asset for asset in manifest['assets'] if asset['package_type'] == 'app.zip')
        item = ET.parse(self.output / 'appcast.xml').find('channel/item')
        self.assertEqual(item.findtext(f'{{{signing.SPARKLE_NS}}}version'), '12')
        self.assertEqual(item.findtext(f'{{{signing.SPARKLE_NS}}}channel'), 'rc')
        self.assertEqual(item.find('enclosure').get('url'), archive['url'])
        self.assertEqual(item.find('enclosure').get('length'), str(archive['bytes']))
        envelope = json.loads((self.output / 'release.json.sig').read_bytes())
        self.assertEqual(envelope, {'schema_version': 1, 'algorithm': 'ed25519', 'key_id': 'fixture',
                                    'signature': (b's' * 64).hex()})
        self.assertEqual(self.operations.count('Ed25519 verification'), 2)
        self.assertIn('Appcast verification', self.operations)
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.run_signer(self.checked)

    def test_failed_key_binding_or_feed_verification_does_not_publish_outputs(self):
        original = self.keys.read_text()
        self.keys.write_text(original.replace(self.sparkle_seed.hex(), (b'\x07' * 32).hex()))
        with self.assertRaisesRegex(ValueError, 'Sparkle signing key does not match'):
            self.run_signer(self.checked)
        self.assertFalse(self.output.exists())
        self.assertNotIn('Ed25519 signing', self.operations)
        self.keys.write_text(original)

        def fail_verify(arguments, operation, *, data):
            if operation == 'Appcast verification':
                raise ValueError('Appcast verification failed')
            return self.checked(arguments, operation, data=data)

        with self.assertRaisesRegex(ValueError, 'Appcast verification failed'):
            self.run_signer(fail_verify)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob('.sign-release-*')), [])
