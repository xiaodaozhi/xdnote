"""Check real development identity and generated header freshness."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_version import identity, release_identity, write_changed


class BuildVersionTests(unittest.TestCase):
    def test_release_requires_clean_commit_and_increasing_version_and_build(self):
        record = {'schemaVersion': 1, 'product': 'xdnote', 'version': '0.2.0-rc.1', 'build': 12,
                  'revision': 'a' * 40, 'previousVersion': '0.2.0-beta.9', 'previousBuild': 11}
        result = release_identity(record, 'a' * 40, False, 'Release')
        self.assertEqual((result['channel'], result['build']), ('rc', 12))
        for change in ({'build': 11}, {'build': True}, {'version': '0.2.0-beta.8'}, {'version': '0.2.0+build'}, {'revision': 'b' * 40}, {'extra': 1}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                release_identity(record | change, 'a' * 40, False, 'Release')
        for dirty, configuration in ((True, 'Release'), (False, 'Debug')):
            with self.assertRaises(ValueError): release_identity(record, 'a' * 40, dirty, configuration)

    def test_source_archive_does_not_invent_a_revision_or_release_counter(self):
        with tempfile.TemporaryDirectory() as directory:
            value = identity(directory, '0.1.0', 'Release')
            self.assertEqual(value['revision'], 'unknown')
            self.assertEqual(value['version'], '0.1.0-dev.unknown')
            self.assertEqual(value['build'], 0)
            self.assertTrue(value['dirty'])
        for base in ('v0.1.0', '0.01.0', '65536.0.0', '0.1.0-alpha.1', 'bad'):
            with self.subTest(base=base), self.assertRaises(ValueError): identity('.', base, 'Release')

    def test_clean_commit_dirty_changes_and_nested_directory_are_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / '开发工作区'; root.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL, text=True, encoding='utf-8').strip()
            git('init'); git('config', 'user.name', 'Fixture'); git('config', 'user.email', 'fixture@example.invalid')
            (root / 'source').write_text('own sample')
            git('add', 'source'); git('commit', '-m', 'fixture')
            expected = git('rev-parse', 'HEAD')
            clean = identity(root, '0.1.0', 'Debug')
            self.assertEqual(clean['revision'], expected)
            self.assertEqual(clean['version'], '0.1.0-dev.' + expected[:12])
            self.assertFalse(clean['dirty'])
            (root / 'source').write_text('changed')
            self.assertTrue(identity(root, '0.1.0', 'Debug')['dirty'])
            (root / 'nested').mkdir()
            self.assertEqual(identity(root / 'nested', '0.1.0', 'Release')['revision'], 'unknown')

    def test_unchanged_header_preserves_mtime_and_changed_bytes_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'generated' / 'BuildVersion.h'
            write_changed(path, b'original')
            timestamp = path.stat().st_mtime_ns
            write_changed(path, b'original')
            self.assertEqual(path.stat().st_mtime_ns, timestamp)
            write_changed(path, b'changed')
            self.assertEqual(path.read_bytes(), b'changed')


if __name__ == '__main__': unittest.main()
