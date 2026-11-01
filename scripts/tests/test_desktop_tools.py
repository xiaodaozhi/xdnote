"""Guard the build entry point's evidence and output boundaries."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "desktop.py"
SPEC = importlib.util.spec_from_file_location("desktop", MODULE)
desktop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(desktop)


class DesktopToolTests(unittest.TestCase):
    def test_build_output_must_stay_in_dedicated_build_subdirectory(self):
        with self.assertRaises(ValueError):
            desktop.build_path(desktop.ROOT / "apps")
        with self.assertRaises(ValueError):
            desktop.build_path(desktop.BUILD)
        with self.assertRaises(ValueError):
            desktop.build_path(desktop.BUILD / ".." / "escaped")
        self.assertEqual(desktop.build_path(desktop.BUILD / "tool-test"),
                         (desktop.BUILD / "tool-test").resolve())

    def test_qt_critical_diagnostic_fails_even_with_zero_exit_status(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                desktop.run([sys.executable, "-c", "print('QCRITICAL : broken native contract')"], os.environ.copy())

    def test_process_failure_cannot_be_hidden_by_log_output(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                desktop.run([sys.executable, "-c", "raise SystemExit(3)"], os.environ.copy())

    def test_log_preserves_utf8_diagnostics(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="tool-test-", dir=desktop.BUILD) as directory:
            report = Path(directory) / "result.log"
            env = os.environ.copy()
            env["PYTHONIOENCODING"] = "utf-8"
            with contextlib.redirect_stdout(io.StringIO()):
                desktop.run([sys.executable, "-c", "print('中文诊断')"], env, report)
            self.assertEqual(report.read_text(encoding="utf-8"), "中文诊断\n")


if __name__ == "__main__":
    unittest.main()
