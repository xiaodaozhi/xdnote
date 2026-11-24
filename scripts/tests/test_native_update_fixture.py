"""Private source instrumentation refuses ambiguous or stale patch inputs."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import check_native_update as fixture
import check_update_helper as helper
import check_native_product_update as product


class NativeFixtureInputs(unittest.TestCase):
    def test_system_installer_fixtures_refuse_an_ordinary_account(self):
        with patch.dict(helper.os.environ, {'CI': 'false', 'GITHUB_ACTIONS': 'true'}):
            with self.assertRaisesRegex(RuntimeError, 'Disposable'):
                helper.guard()

    def test_private_source_patch_requires_one_exact_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'input.cpp'
            for text in ('unrelated','token token'):
                path.write_text(text)
                with self.assertRaises(ValueError):fixture.replace(path,'token','replacement')
                self.assertEqual(path.read_text(),text)
            path.write_text('before token after');fixture.replace(path,'token','replacement')
            self.assertEqual(path.read_text(),'before replacement after')

    def test_product_fixture_patches_current_launcher(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / 'scripts').mkdir()
            launcher = source / 'scripts/desktop.py'
            launcher.write_bytes((Path(__file__).resolve().parents[1] / 'desktop.py').read_bytes())
            candidate = product.Product.__new__(product.Product)
            candidate.source = source
            candidate.patch_launcher()
            text = launcher.read_text(encoding='utf-8')
            self.assertIn('if args.fixture_installation:', text)
            self.assertIn('stop_at_process_exit=True', text)
            compile(text, str(launcher), 'exec')


if __name__=='__main__':unittest.main()
