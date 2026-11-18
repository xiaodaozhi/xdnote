"""The two Cocoa replacements and their original seal form one transaction."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import TestCase
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import qt_cocoa_backports as cocoa
from installer_common import ROOT, digest


class CocoaPreparationTests(TestCase):
    def setUp(self):
        build = ROOT / 'build'
        build.mkdir(exist_ok=True)
        directory = tempfile.TemporaryDirectory(prefix='cocoa-transaction-', dir=build)
        self.addCleanup(directory.cleanup)
        self.folder = Path(directory.name)
        self.qt = self.folder / 'sdk'
        self.output = self.folder / 'recipe'
        for root, files in ((self.qt, (*cocoa.MODULES, cocoa.SEAL)),
                            (self.output / 'build', cocoa.MODULES)):
            for name in files:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((root.name + ':' + name).encode())
        self.receipt = {'inputs': {}, 'source': {}, 'patches': [], 'compatibility': {},
                        'variant': 'xdnote-cocoa.1'}
        (self.output / 'build-receipt.json').write_text(json.dumps(self.receipt))
        self.originals = {name: digest(self.qt / name) for name in (*cocoa.MODULES, cocoa.SEAL)}

    def signer(self, args, **kwargs):
        if args[0].endswith('codesign') and '--force' in args and args[-1].endswith('QtGui.framework'):
            (self.qt / cocoa.SEAL).write_bytes(b'new seal')

    def verify_installed_receipt(self, qt):
        receipt = json.loads((qt / cocoa.RECEIPT).read_bytes())
        cocoa.verify_installed(qt, receipt)
        return receipt

    def test_signed_pair_can_be_restored_as_a_unit(self):
        with patch.object(cocoa.subprocess, 'run', side_effect=self.signer), \
                patch.object(cocoa, 'verify', side_effect=self.verify_installed_receipt):
            receipt = cocoa.install(self.qt, self.output, self.receipt)
        self.assertEqual(set(receipt['files']), set(cocoa.MODULES))
        for name in cocoa.MODULES:
            self.assertNotEqual(digest(self.qt / name), self.originals[name])
        self.assertNotIn('originalBackups', cocoa.public_from_receipt(receipt))
        cocoa.restore(self.qt)
        self.assertFalse((self.qt / cocoa.RECEIPT).exists())
        self.assertEqual({name: digest(self.qt / name) for name in self.originals}, self.originals)

    def test_second_binary_signing_failure_restores_both_originals_and_the_seal(self):
        def fail_plugin(args, **kwargs):
            if args[0].endswith('codesign') and '--force' in args and args[-1].endswith('libqcocoa.dylib'):
                raise subprocess.CalledProcessError(1, args)
            self.signer(args, **kwargs)

        with patch.object(cocoa.subprocess, 'run', side_effect=fail_plugin):
            with self.assertRaises(subprocess.CalledProcessError):
                cocoa.install(self.qt, self.output, self.receipt)
        self.assertFalse((self.qt / cocoa.RECEIPT).exists())
        self.assertEqual({name: digest(self.qt / name) for name in self.originals}, self.originals)
