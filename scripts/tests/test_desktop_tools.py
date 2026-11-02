"""Guard the build entry point's evidence and output boundaries."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import json

MODULE = Path(__file__).resolve().parents[1] / "desktop.py"
SPEC = importlib.util.spec_from_file_location("desktop", MODULE)
desktop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(desktop)


class DesktopToolTests(unittest.TestCase):
    def test_vcvars_import_keeps_one_case_insensitive_windows_path(self):
        with patch.object(desktop.platform, "system", return_value="Windows"), \
             patch.object(desktop.os, "environ", {"PATH": "before", "SystemRoot": "C:/Windows"}), \
             patch.object(desktop.subprocess, "check_output", return_value="Path=compiler;工具\nSYSTEMROOT=C:/Windows\n"):
            env = desktop.environment(MODULE)
        self.assertEqual([key for key in env if key.lower() == "path"], ["PATH"])
        self.assertEqual(env["PATH"], "compiler;工具")

    def test_compatibility_profile_is_explicit_and_windows_only(self):
        self.assertEqual(desktop.profile_settings("default", "Linux")["qtVersion"], "6.11.3")
        self.assertEqual(desktop.profile_settings("msvc2019", "Windows")["qtVersion"], "6.8.3")
        self.assertTrue(desktop.profile_settings("msvc2019", "Windows")["compatibilityOnly"])
        with self.assertRaises(ValueError):
            desktop.profile_settings("msvc2019", "Darwin")
        with self.assertRaises(ValueError):
            desktop.profile_settings("unknown", "Windows")

    def test_sdk_profile_and_source_mismatch_prevent_running_old_build(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="identity-test-", dir=desktop.BUILD) as directory:
            build = Path(directory)
            qt = Path(directory) / "sdk"
            identity = {"profile": "msvc2019", "qtPrefix": str(qt), "sourceSha256": "current"}
            (build / "build-identity.json").write_text(json.dumps(identity), encoding="utf-8")
            with patch.object(desktop, "source_fingerprint", return_value="current"):
                desktop.verify_build_identity(build, "msvc2019", qt)
                with self.assertRaises(RuntimeError):
                    desktop.verify_build_identity(build, "default", qt)
                with self.assertRaises(RuntimeError):
                    desktop.verify_build_identity(build, "msvc2019", qt / "another")
            with patch.object(desktop, "source_fingerprint", return_value="changed"):
                with self.assertRaises(RuntimeError):
                    desktop.verify_build_identity(build, "msvc2019", qt)

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
