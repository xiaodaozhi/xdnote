"""Verify bounded Sparkle inputs and original helper/resource identity."""
import copy
import hashlib
import io
import json
from pathlib import Path
import plistlib
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sparkle
import sparkle_build


class SparkleAdmissionTests(unittest.TestCase):
    def setUp(self):
        (sparkle.ROOT / 'build').mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=sparkle.ROOT / 'build')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def archive(self, members):
        path = self.root / 'fixture.tar'
        with tarfile.open(path, 'w') as output:
            for name, kind, payload in members:
                member = tarfile.TarInfo(name)
                if kind == 'file':
                    member.size = len(payload)
                    output.addfile(member, io.BytesIO(payload))
                else:
                    member.type = tarfile.SYMTYPE if kind == 'link' else tarfile.LNKTYPE
                    member.linkname = payload
                    output.addfile(member)
        return path

    def test_extraction_rejects_escape_duplicates_link_traversal_and_hardlinks(self):
        cases = [
            [('../outside', 'file', b'x')], [('/absolute', 'file', b'x')],
            [('same', 'file', b'x'), ('./same', 'file', b'y')],
            [('escape', 'link', '../../outside')],
            [('alias', 'link', 'inside'), ('alias/content', 'file', b'x')],
            [('hard', 'hardlink', 'inside')],
        ]
        for index, members in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ValueError):
                sparkle.extract(self.archive(members), self.root / f'extract-{index}', links=True)
        self.assertFalse((self.root.parent / 'outside').exists())

    @unittest.skipIf(sys.platform == 'win32', 'Framework symlinks are macOS-specific; containment is covered without creating links')
    def test_internal_framework_links_are_preserved_and_broken_links_rejected(self):
        archive = self.archive([('Versions/B/header', 'file', b'fixture'), ('Versions/Current', 'link', 'B'), ('Headers', 'link', 'Versions/Current')])
        root = self.root / 'sdk'
        sparkle.extract(archive, root, links=True)
        self.assertEqual((root / 'Headers/header').read_bytes(), b'fixture')
        self.assertEqual(sparkle.inventory(root)['Versions/Current'], {'link': 'B'})
        archive = self.archive([('broken', 'link', 'absent')])
        with self.assertRaises(OSError):
            sparkle.extract(archive, self.root / 'broken', links=True)

    def test_material_is_offline_by_default_and_mismatched_bytes_never_replace(self):
        spec = {'file': 'test-sparkle-material', 'sha256': hashlib.sha256(b'fixture').hexdigest(), 'url': 'https://example.invalid/fixture'}
        with patch.object(sparkle, 'ROOT', self.root), patch('urllib.request.urlopen') as network:
            with self.assertRaisesRegex(ValueError, 'explicitly prepare'):
                sparkle.material(spec, False)
            network.assert_not_called()
            target = self.root / 'build/source-cache' / spec['file']
            target.parent.mkdir(parents=True)
            target.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'checksum differs'):
                sparkle.material(spec, True)
            self.assertEqual(target.read_bytes(), b'changed')
            network.assert_not_called()

    def test_inputs_and_output_paths_cannot_borrow_another_project(self):
        with self.assertRaises(ValueError): sparkle.owned(self.root.parent.parent / 'other-project/build/sdk')
        self.assertEqual(sparkle.owned(self.root / 'sdk'), self.root / 'sdk')
        self.assertEqual(sparkle.configuration()['framework_build']['patch_sha256'], sparkle.digest(sparkle.ROOT / 'patches/sparkle/2.10.0-download-proxy.patch'))

    def fixture(self):
        spec = copy.deepcopy(sparkle.configuration())
        spec['license_sha256'] = hashlib.sha256(b'fixture license').hexdigest()
        sdk = self.root / 'input/sdk'
        names = ('LICENSE', 'Sparkle.framework/Versions/B/Sparkle', 'Sparkle.framework/Versions/B/Headers/SPUUpdater.h',
                 'Sparkle.framework/Versions/B/Autoupdate', 'Sparkle.framework/Versions/B/Updater.app/Contents/MacOS/Updater',
                 'Sparkle.framework/Versions/B/XPCServices/Downloader.xpc/Contents/MacOS/Downloader',
                 'Sparkle.framework/Versions/B/XPCServices/Installer.xpc/Contents/MacOS/Installer')
        for name in names:
            path = sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture license' if name == 'LICENSE' else b'non-executable own fixture')
        info = sdk / 'Sparkle.framework/Versions/B/Resources/Info.plist'
        info.parent.mkdir(parents=True)
        info.write_bytes(plistlib.dumps({'CFBundleShortVersionString': spec['version']}))
        receipt = {'dependency': spec, 'files': sparkle.inventory(sdk.parent)}
        (sdk.parent / sparkle.RECEIPT).write_text(json.dumps(receipt), encoding='utf-8')
        return sdk.parent, spec

    def test_receipt_detects_changed_missing_and_added_helpers(self):
        root, spec = self.fixture()
        with patch.object(sparkle, 'configuration', return_value=spec):
            sparkle.verify(root)
            target = root / 'sdk/Sparkle.framework/Versions/B/Autoupdate'
            before = target.read_bytes()
            target.write_bytes(b'changed helper')
            with self.assertRaisesRegex(ValueError, 'identity differs'): sparkle.verify(root)
            target.write_bytes(before)
            addition = root / 'sdk/unrecorded'
            addition.write_bytes(b'unknown')
            with self.assertRaisesRegex(ValueError, 'identity differs'): sparkle.verify(root)
            addition.unlink(); target.unlink()
            # Even a recomputed local receipt cannot omit required helpers.
            receipt = {'dependency': spec, 'files': sparkle.inventory(root)}
            (root / sparkle.RECEIPT).write_text(json.dumps(receipt), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'incomplete'): sparkle.verify(root)

    def test_source_phase_parser_rejects_escaped_or_unreviewed_compiler_inputs(self):
        root = self.root / 'source'; root.mkdir()
        (root / 'unit.m').write_text('/* own fixture */')
        project = {'objects': {'target': {'isa': 'PBXNativeTarget', 'name': 'Sparkle', 'buildPhases': ['phase']},
            'phase': {'isa': 'PBXSourcesBuildPhase', 'files': ['entry']}, 'entry': {'fileRef': 'file'},
            'file': {'sourceTree': 'SOURCE_ROOT', 'path': 'unit.m'}}}
        self.assertEqual(sparkle_build.source_files(project, 'Sparkle', root), [root / 'unit.m'])
        project['objects']['entry']['settings'] = {'COMPILER_FLAGS': '-DFOREIGN'}
        with self.assertRaisesRegex(ValueError, 'compiler settings'): sparkle_build.source_files(project, 'Sparkle', root)
        project['objects']['entry'].pop('settings')
        project['objects']['file']['path'] = '../escaped.m'
        with self.assertRaisesRegex(ValueError, 'escapes'): sparkle_build.source_files(project, 'Sparkle', root)


if __name__ == '__main__':
    unittest.main()
