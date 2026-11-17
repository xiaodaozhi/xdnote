"""Private source instrumentation refuses ambiguous or stale patch inputs."""
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import check_native_update as fixture


class NativeFixtureInputs(unittest.TestCase):
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
