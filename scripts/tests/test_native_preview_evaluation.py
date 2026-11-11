"""Guard the native evaluation's Chromium exclusion without launching a GUI."""
import importlib.util
from pathlib import Path
import tempfile
import json
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("native_evaluate", ROOT / "experiments/native-preview-spike/evaluate.py")
evaluation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluation)


class NativePreviewEvaluationTests(unittest.TestCase):
    def test_direct_and_transitive_browser_imports_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            qt = Path(folder)
            (qt / "bin").mkdir()
            for name in ("Qt6Core.dll", "Qt6Widgets.dll"):
                (qt / "bin" / name).touch()
            for dependency in ("Qt6WebEngineCore.dll", "WebView2Loader.dll", "libcef.dll", "chrome_elf.dll"):
                def imports(command, **kwargs):
                    return "    Qt6Widgets.dll\n" if Path(command[-1]).name == "native.exe" else "    " + dependency + "\n"
                with self.subTest(dependency=dependency), patch.object(evaluation.subprocess, "check_output", side_effect=imports):
                    with self.assertRaisesRegex(RuntimeError, "Excluded browser dependency"):
                        evaluation.qt_closure(qt / "native.exe", qt, "dumpbin", {})

    def test_native_closure_handles_cycles_and_localized_headings(self):
        with tempfile.TemporaryDirectory() as folder:
            qt = Path(folder); (qt / "bin").mkdir()
            for name in ("Qt6Core.dll", "Qt6Widgets.dll"):
                (qt / "bin" / name).write_bytes(b"fixture")
            def imports(command, **kwargs):
                name = Path(command[-1]).name
                return "依赖项\n    " + ("Qt6Widgets.dll" if name == "native.exe" else "Qt6Core.dll") + "\n"
            with patch.object(evaluation.subprocess, "check_output", side_effect=imports):
                values = evaluation.qt_closure(qt / "native.exe", qt, "dumpbin", {})
            self.assertEqual({row["name"] for row in values}, {"Qt6Core.dll", "Qt6Widgets.dll"})
            self.assertEqual(sum(row["bytes"] for row in values), 14)

    def test_cmake_does_not_request_a_browser_module(self):
        content = (ROOT / "experiments/native-preview-spike/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("REQUIRED COMPONENTS Widgets", content)
        for token in ("WebEngine", "WebView", "Chromium", "CEF"):
            self.assertNotIn(token, content)

    def test_product_build_and_sdk_exclude_browser_modules(self):
        for path in (ROOT / "CMakeLists.txt", ROOT / "cmake/NativePreviewView.cmake"):
            text = path.read_text(encoding="utf-8")
            for token in ("WebEngine", "WebView", "CEF", "Chromium"):
                self.assertNotIn(token, text, str(path))
        modules = json.loads((ROOT / "tools/desktop-dependencies.json").read_text(encoding="utf-8"))["qt"]["modules"]
        self.assertFalse(any("webengine" in name.lower() or "webview" in name.lower() for name in modules))


if __name__ == "__main__":
    unittest.main()
