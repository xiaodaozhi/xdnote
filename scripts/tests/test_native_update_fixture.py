"""Private source instrumentation refuses ambiguous or stale patch inputs."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import check_native_update as fixture
import check_update_helper as helper


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


if __name__=='__main__':unittest.main()
