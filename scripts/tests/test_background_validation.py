"""Protect noninteractive execution and evidence classification."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("background_validation", Path(__file__).resolve().parents[1] / "background_validation.py")
background = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(background)


class BackgroundTests(unittest.TestCase):
    def test_offscreen_environment_cannot_inherit_native_platform(self):
        with tempfile.TemporaryDirectory(dir=background.ROOT / "build") as directory:
            source = {"QT_QPA_PLATFORM": "windows", "PATH": "keep", "QT_SCALE_FACTOR": "2"}
            env = background.offscreen_environment(source, Path(directory))
            self.assertTrue(env["QT_QPA_PLATFORM"].startswith("offscreen:configfile=build/"))
            self.assertNotIn(":", env["QT_QPA_PLATFORM"].split("configfile=")[1])
            self.assertEqual(env["QT_SCALE_FACTOR"], "1")
            self.assertEqual(env["PATH"], "keep")
            self.assertEqual(source["QT_QPA_PLATFORM"], "windows")

    def test_skips_missing_summary_and_fatal_diagnostics_are_not_success(self):
        self.assertEqual(background.check_report("Totals: 4 passed, 0 failed, 0 skipped, 10ms"), 4)
        for text in ("", "Totals: 4 passed, 0 failed, 1 skipped", "Totals: 4 passed, 1 failed, 0 skipped",
                     "QCRITICAL : broken\nTotals: 4 passed, 0 failed, 0 skipped"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                background.check_report(text)

    def test_large_input_is_only_given_to_bounded_rejection_paths(self):
        cases = list(background.cases())
        self.assertEqual(len(cases), len(set(cases)))
        large = {operation for operation, size, _ in cases if size > 2 * 1024 * 1024}
        self.assertEqual(large, {"render", "references", "headings", "file"})
        self.assertTrue(any(item == ("cancel", 0, "generated") for item in cases))
        functions = " ".join(function for group in background.GROUPS.values() for function in group)
        self.assertNotIn("UIA", functions)
        self.assertNotIn("FilePicker", functions)
        self.assertNotIn("mixedDpiNative", functions)


if __name__ == "__main__":
    unittest.main()
