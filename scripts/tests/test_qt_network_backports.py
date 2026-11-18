"""Prepared source variants must bind SDK bytes and survive failed installation."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import qt_network_backports as network
from installer_common import digest, ROOT


class QtNetworkPreparation(unittest.TestCase):
    def test_receipt_rejects_changed_sdk_original_backup_and_core_configuration(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory, patch.object(network.platform, 'system', return_value='Linux'):
            sdk = Path(directory) / 'sdk'; original = Path(directory) / 'original'
            file = sdk / 'lib/libQt6Network.so.6.8.2'; file.parent.mkdir(parents=True); file.write_bytes(b'prepared')
            original.write_bytes(b'supplier')
            core = network.headers(sdk, 'Core')
            core_files = [core / 'qconfig.h', core / '6.8.2/QtCore/private/qconfig_p.h', core / '6.8.2/QtCore/private/qtcore-config_p.h']
            for p in core_files: p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'core configuration')
            record = {'configurationSha256': digest(ROOT / 'tools/qt-network-backports.json'), 'variant': network.config()['variant'],
                      'producerSha256': digest(Path(network.__file__)), 'compilerCompatibilitySha256': digest(ROOT / 'cmake/QtCompilerCompatibility.h'),
                      'files': {'lib/libQt6Network.so.6.8.2': {'sha256': digest(file), 'originalSdkSha256': digest(original), 'originalBackup': str(original)}},
                      'coreConfiguration': {str(p.relative_to(sdk)): digest(p) for p in core_files}}
            (sdk / network.RECEIPT).write_text(json.dumps(record))
            self.assertEqual(network.verify(sdk), record)
            self.assertNotIn('originalBackup', str(network.public_record(sdk)))
            for target in [file, original, *core_files]:
                before = target.read_bytes(); target.write_bytes(before + b'changed')
                with self.assertRaises(ValueError): network.verify(sdk)
                target.write_bytes(before)
            changed = {**record, 'configurationSha256': '0'*64}; (sdk / network.RECEIPT).write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'stale'): network.verify(sdk)

    def test_failed_second_replacement_restores_original_sdk_and_cleans_lock(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory, patch.object(network.platform, 'system', return_value='Windows'):
            sdk = Path(directory) / 'sdk'; output = Path(directory) / 'cache'; output.mkdir()
            files = {}
            for name in network.module_paths('Windows'):
                target = sdk / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(b'original '+name.encode())
                built = output / 'build' / name; built.parent.mkdir(parents=True, exist_ok=True); built.write_bytes(b'modified '+name.encode())
                files[name] = {'builtFile': 'build/'+name, 'sha256': digest(built)}
            copy = network.shutil.copy2
            def fail_second(source, destination, **kwargs):
                if Path(destination).name == 'Qt6Network.lib.xdnote-new': raise OSError('owned fixture copy failure')
                return copy(source, destination, **kwargs)
            with patch.object(network.shutil, 'copy2', side_effect=fail_second), self.assertRaises(OSError):
                network.install(sdk, output, {'files': files})
            for name in files: self.assertEqual((sdk / name).read_bytes(), b'original '+name.encode())
            self.assertFalse((sdk / network.RECEIPT).exists()); self.assertFalse((sdk / '.xdnote-network-prepare-lock').exists())
            self.assertFalse(list(sdk.rglob('*.xdnote-new')))

    def test_explicit_restore_preserves_unrelated_edits_and_rejects_escaping_paths(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory, patch.object(network.platform, 'system', return_value='Linux'):
            sdk = Path(directory) / 'sdk'; backup = Path(directory) / 'backup'
            target = sdk / network.module_paths('Linux')[0]; target.parent.mkdir(parents=True)
            target.write_bytes(b'prepared'); backup.write_bytes(b'supplier')
            record = {'files': {target.relative_to(sdk).as_posix(): {'sha256': digest(target),
                'originalSdkSha256': digest(backup), 'originalBackup': str(backup)}}}
            (sdk / network.RECEIPT).write_text(json.dumps(record))
            target.write_bytes(b'user change')
            with self.assertRaisesRegex(ValueError, 'unrelated changes'): network.restore(sdk)
            self.assertEqual(target.read_bytes(), b'user change')
            target.unlink(); target.symlink_to(backup)
            with self.assertRaisesRegex(ValueError, 'escapes'): network.restore(sdk)
            target.unlink(); target.write_bytes(b'prepared')
            network.restore(sdk)
            self.assertEqual(target.read_bytes(), b'supplier')
            self.assertFalse((sdk / network.RECEIPT).exists())


if __name__ == '__main__': unittest.main()
