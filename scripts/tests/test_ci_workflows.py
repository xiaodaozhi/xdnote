"""Protect routine and on-demand Actions entry points from accidental drift."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github/workflows"


def events(path):
    text = path.read_text(encoding="utf-8")
    match = re.search(r"(?ms)^on:\s*\n(.*?)(?=^[^ \t#\n])", text)
    if not match:
        raise AssertionError(f"Missing on block in {path}")
    return set(re.findall(r"^  ([a-z_]+):", match.group(1), re.MULTILINE))


class WorkflowContractTests(unittest.TestCase):
    def test_routine_workflow_is_single_linux_job(self):
        path = WORKFLOWS / "materials.yml"
        text = path.read_text(encoding="utf-8")
        self.assertEqual(events(path), {"push", "pull_request", "workflow_dispatch"})
        self.assertIn("branches: [main]", text)
        self.assertEqual(re.findall(r"^  ([a-z_]+):\s*$", text.split("\njobs:\n", 1)[1], re.MULTILINE), ["materials"])
        self.assertIn("name: Materials (ubuntu-24.04)", text)
        self.assertIn("runs-on: ubuntu-24.04", text)
        self.assertNotIn("matrix:", text)
        self.assertIn("python -m unittest discover -s scripts/tests", text)

    def test_full_checks_are_manual_only(self):
        ordinary = {p.name for p in WORKFLOWS.glob("*.yml") if "pull_request" in events(p) or "push" in events(p)}
        self.assertEqual(ordinary, {"materials.yml"})
        for name in ("desktop-admission.yml", "qt-document-spike.yml", "scintilla-buffer-spike.yml", "pulldown-markdown-spike.yml"):
            with self.subTest(workflow=name):
                self.assertEqual(events(WORKFLOWS / name), {"workflow_dispatch"})


if __name__ == "__main__":
    unittest.main()
