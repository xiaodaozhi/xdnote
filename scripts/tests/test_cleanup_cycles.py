"""Exercise cleanup boundaries with disposable fixtures only."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("cleanup_cycles", Path(__file__).resolve().parents[1] / "cleanup_cycles.py")
cleanup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cleanup)


class CleanupTests(unittest.TestCase):
    def setUp(self):
        fixture_root = Path(__file__).resolve().parents[2] / "build"
        fixture_root.mkdir(exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix="cleanup-test-", dir=fixture_root)
        self.addCleanup(self.tmp.cleanup)
        self.build = Path(self.tmp.name) / "build"
        self.build.mkdir()
        self.build_patch = patch.object(cleanup, "BUILD", self.build)
        self.build_patch.start()
        self.addCleanup(self.build_patch.stop)
        self.worktree_patch = patch.object(cleanup, "worktrees", return_value=set())
        self.worktree_patch.start()
        self.addCleanup(self.worktree_patch.stop)

    def run_cli(self, *args):
        output = io.StringIO()
        with patch.object(sys, "argv", ["cleanup_cycles.py", *args]), contextlib.redirect_stdout(output):
            cleanup.main()
        return output.getvalue()

    def marked(self, name):
        self.run_cli("--init", name)
        return self.build / name

    def test_preview_and_explicit_delete(self):
        path = self.marked("cycle-example")
        (path / "result.txt").write_text("temporary", encoding="utf-8")
        self.run_cli("--path", path.name)
        self.assertTrue(path.exists())
        self.run_cli("--path", path.name, "--apply", "--yes")
        self.assertFalse(path.exists())

    def test_unmarked_existing_or_outside_directory_is_never_adopted(self):
        path = self.build / "cycle-existing"
        path.mkdir()
        for args in [("--init", path.name), ("--path", path.name), ("--path", "../other"), ("--path", "Qt")]:
            with self.subTest(args=args), self.assertRaises(SystemExit):
                self.run_cli(*args)
        self.assertTrue(path.exists())

    def test_active_worktree_and_its_parent_are_protected(self):
        path = self.marked("cycle-active")
        with patch.object(cleanup, "worktrees", return_value={path / "nested-worktree"}):
            with self.assertRaises(SystemExit):
                self.run_cli("--path", path.name, "--apply", "--yes")
        self.assertTrue(path.exists())
        # The main worktree containing build is not itself an active nested worktree.
        self.assertEqual(cleanup.candidate(path.name, {self.build.parent.resolve()}), path.resolve())

    def test_stale_selects_only_marked_inactive_unchanged_cycles(self):
        old = self.marked("cycle-old")
        recent = self.marked("cycle-recent")
        unmarked = self.build / "cycle-unknown"
        unmarked.mkdir()
        stamp = time.time() - 40 * 86400
        os.utime(old / cleanup.MARKER, (stamp, stamp))
        os.utime(old, (stamp, stamp))
        self.run_cli("--stale-days", "30", "--apply", "--yes")
        self.assertFalse(old.exists())
        self.assertTrue(recent.exists())
        self.assertTrue(unmarked.exists())

    def test_nested_link_refused(self):
        path = self.marked("cycle-link")
        try:
            (path / "link").symlink_to(self.build, target_is_directory=True)
        except OSError:
            self.skipTest("symlink creation unavailable")
        with self.assertRaises(SystemExit):
            self.run_cli("--path", path.name, "--apply", "--yes")
        self.assertTrue(path.exists())


if __name__ == "__main__":
    unittest.main()
