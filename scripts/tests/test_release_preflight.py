"""Preflight preserves the distinction between bytes, authenticity and release."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_preflight as preflight


class ReleasePreflightTests(unittest.TestCase):
    def inputs(self, root):
        (root / 'tools').mkdir(); (root / 'tools/update-trust.json').write_text('{"schema_version":1,"keys":[]}\n'); (root / 'tools/sparkle-update-key.json').write_text('{"publicKey":""}\n')
        identity = dict(product='xdnote', version='0.2.0', build=12, revision='a'*40, dirty=False, channel='stable', configuration='Release')
        assets = root / 'assets'; assets.mkdir(); verification = {'identity': identity, 'assets': {}}
        for *_, suffix, _minimum in preflight.release_candidate.TARGETS:
            name = 'xdnote-0.2.0-' + suffix; data = ('unit fixture ' + name).encode(); (assets / name).write_bytes(data)
            verification['assets'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'verified': True}
        return identity, assets, verification

    def test_unsigned_metadata_success_keeps_production_and_release_gates_blocked(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); identity, assets, evidence = self.inputs(root)
            with patch.object(preflight, 'ROOT', root): result = preflight.preflight(identity, assets, evidence)
            self.assertTrue(result['ok']); self.assertFalse(result['authenticated']); self.assertFalse(result['releaseReady']); self.assertFalse(result['licenseReviewComplete'])
            self.assertIn('production_update_root_missing', result['blockers']); self.assertIn('original_manifest_not_authenticated', result['blockers'])
            (assets / result['candidate']['assets'][0]['filename']).write_bytes(b'changed')
            with patch.object(preflight, 'ROOT', root), self.assertRaises(ValueError): preflight.preflight(identity, assets, evidence)

    def test_verifier_must_bind_original_bytes_and_compiled_root_with_consistent_status(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); identity, assets, evidence = self.inputs(root); build = root / 'build'; build.mkdir(); executable = build / 'verifier'; executable.write_bytes(b'unit fixture, never executed')
            manifest = root / 'manifest.json'; manifest.write_text(json.dumps(preflight.release_candidate.manifest(identity, assets, evidence))); signature = root / 'signature.json'; signature.write_text('{}')
            status = {'ok': False, 'authenticated': False, 'error': 'untrusted_signing_key', 'trustRootSha256': preflight.digest(root / 'tools/update-trust.json'), 'manifestSha256': preflight.digest(manifest)}
            with patch.object(preflight, 'ROOT', root), patch.object(preflight.desktop, 'checked_tool_path', return_value=executable), patch.object(preflight.desktop, 'repository_owner', return_value=(root, root / '.git')):
                def run(value, code=2):
                    with patch.object(preflight.subprocess, 'run', return_value=subprocess.CompletedProcess([], code, json.dumps(value).encode(), b'')):
                        return preflight.preflight(identity, assets, evidence, manifest, signature, executable)
                result = run(status); self.assertFalse(result['authenticated']); self.assertEqual(result['authenticationError'], 'untrusted_signing_key')
                for change in [{'trustRootSha256': 'stale'}, {'manifestSha256': 'stale'}, {'authenticated': 1}, {'ok': True, 'authenticated': True}]:
                    with self.assertRaises(ValueError): run(status | change, 0 if change.get('authenticated') is True else 2)
                with self.assertRaises(ValueError): preflight.preflight(identity, assets, evidence, manifest)
