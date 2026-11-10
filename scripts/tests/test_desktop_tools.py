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
    def test_runtime_environment_is_local_and_rejects_missing_dlls_before_launch(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="runtime-", dir=desktop.BUILD) as directory:
            qt = Path(directory)
            (qt / "bin").mkdir()
            original = {"PATH": "existing", "QT_PLUGIN_PATH": "foreign", "QTWEBENGINEPROCESS_PATH": "foreign", "QTWEBENGINE_RESOURCES_PATH": "foreign"}
            with self.assertRaisesRegex(RuntimeError, "Qt runtime is incomplete"):
                desktop.runtime_environment(original, qt, "Windows")
            for name in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6WebEngineCore.dll", "QtWebEngineProcess.exe"):
                (qt / "bin" / name).touch()
            result = desktop.runtime_environment(original, qt, "Windows")
            self.assertEqual(result["PATH"], str(qt / "bin") + os.pathsep + "existing")
            self.assertEqual(result["QT_PLUGIN_PATH"], str(qt / "plugins"))
            self.assertEqual(result["QTWEBENGINEPROCESS_PATH"], str(qt / "bin/QtWebEngineProcess.exe"))
            self.assertNotIn("QTWEBENGINE_RESOURCES_PATH", result)
            self.assertEqual(original["PATH"], "existing")
            self.assertEqual(original["QT_PLUGIN_PATH"], "foreign")

    def test_system_package_receipt_does_not_exempt_arbitrary_checkouts(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="system-package-", dir=desktop.BUILD) as directory:
            prefix = Path(directory)
            (prefix / ".git").mkdir()
            (prefix / "Library/Homebrew").mkdir(parents=True)
            (prefix / "Library/Homebrew/brew.sh").touch()
            package = prefix / "Cellar/rustup/1.0"
            (package / "bin").mkdir(parents=True)
            tool = package / "bin/rustup"
            tool.touch()
            with patch.object(desktop, "SYSTEM_BREW_PREFIXES", (prefix,)):
                with self.assertRaises(ValueError):
                    desktop.checked_tool_path(tool, "rustup")
                (package / "INSTALL_RECEIPT.json").write_text("{}", encoding="utf-8")
                self.assertEqual(desktop.checked_tool_path(tool, "rustup"), tool.resolve())
                source = prefix / "source/tool"
                source.parent.mkdir()
                source.touch()
                with self.assertRaises(ValueError):
                    desktop.checked_tool_path(source, "CMake")
                # A nested foreign checkout remains foreign even inside a package.
                (package / ".git").mkdir()
                with self.assertRaises(ValueError):
                    desktop.checked_tool_path(tool, "rustup")

    def test_tools_reject_foreign_checkout_and_allow_same_project_worktree(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="tool-owner-", dir=desktop.BUILD) as directory:
            fixture = Path(directory)
            project, foreign, worktree = (fixture / name for name in ("project", "foreign", "worktree"))
            for checkout in (project, foreign):
                (checkout / ".git").mkdir(parents=True)
                (checkout / "build").mkdir()
            foreign_tool = foreign / "build/tool.exe"
            foreign_tool.touch()
            with patch.object(desktop, "ROOT", project):
                with self.assertRaisesRegex(ValueError, "another Git repository"):
                    desktop.checked_tool_path(foreign_tool, "CMake")
                with patch.object(desktop.shutil, "which", return_value=str(foreign_tool)):
                    with self.assertRaisesRegex(ValueError, "another Git repository"):
                        desktop.prepared_tool("foreign-cmake", {"PATH": str(foreign_tool.parent)})
                # A linked worktree may share this project's tools, not another project's.
                worktree.mkdir()
                gitdir = project / ".git/worktrees/task"
                gitdir.mkdir(parents=True)
                (gitdir / "commondir").write_text("../..", encoding="utf-8")
                (worktree / ".git").write_text("gitdir: " + str(gitdir), encoding="utf-8")
                own_tool = project / "build/tool.exe"
                own_tool.touch()
                with patch.object(desktop, "ROOT", worktree):
                    self.assertEqual(desktop.checked_tool_path(own_tool, "CMake"), own_tool.resolve())
                    self.assertEqual(desktop.checked_tool_path(project / "build", "Qt", sdk=True), (project / "build").resolve())
                outside_sdk = project / "external-sdk"
                outside_sdk.mkdir()
                with self.assertRaisesRegex(ValueError, "build tree"):
                    desktop.checked_tool_path(outside_sdk, "Qt", sdk=True)
                # Resolving a path link must not conceal foreign ownership.
                link = project / "build/foreign-link"
                try:
                    link.symlink_to(foreign_tool)
                except OSError:
                    pass  # The existing cleanup test separately reports unavailable symlink creation.
                else:
                    with self.assertRaisesRegex(ValueError, "another Git repository"):
                        desktop.checked_tool_path(link, "CMake")

    def test_virtual_environment_tools_precede_inherited_path(self):
        desktop.BUILD.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="tool-launcher-", dir=desktop.BUILD) as directory:
            folder = Path(directory)
            tool = folder / ("cmake.exe" if desktop.platform.system() == "Windows" else "cmake")
            tool.touch()
            with patch.object(desktop.sys, "executable", str(folder / "python")), \
                 patch.object(desktop.shutil, "which", return_value=str(tool)) as which:
                self.assertEqual(desktop.prepared_tool("cmake", {"PATH": "inherited"}), str(tool.resolve()))
                which.assert_called_once_with(str(tool), path="inherited")

    def test_vcvars_import_keeps_one_case_insensitive_windows_path(self):
        with patch.object(desktop.platform, "system", return_value="Windows"), \
             patch.object(desktop.os, "environ", {"PATH": "before", "SystemRoot": "C:/Windows"}), \
             patch.object(desktop.subprocess, "check_output", return_value="Path=compiler;工具\nSYSTEMROOT=C:/Windows\n"):
            env = desktop.environment(MODULE)
        self.assertEqual([key for key in env if key.lower() == "path"], ["PATH"])
        self.assertEqual(env["PATH"], "compiler;工具")

    def test_compatibility_profile_is_explicit_and_windows_only(self):
        for system in ("Windows", "Linux", "Darwin"):
            self.assertEqual(desktop.profile_settings("default", system)["qtVersion"], "6.8.2")
        self.assertEqual(desktop.profile_settings("msvc2019", "Windows")["qtVersion"], "6.8.2")
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
