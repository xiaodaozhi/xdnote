"""Protect routine and on-demand Actions entry points from accidental drift."""
from pathlib import Path
import json
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
    def test_targeted_workspace_scope_keeps_full_matrix_explicit(self):
        text = (WORKFLOWS / "desktop-admission.yml").read_text(encoding="utf-8")
        self.assertIn("options: [full, workspace-linux, resources-unix]", text)
        self.assertIn("default: full", text)
        matrices = [json.loads(value) for value in re.findall(r"'(\[\{[^\n]+?\}\])'", text)]
        self.assertEqual(len(matrices), 3)
        self.assertEqual([entry['os'] for entry in matrices[0]], ['ubuntu-22.04'])
        self.assertEqual([entry['os'] for entry in matrices[1]], ['ubuntu-22.04', 'macos-15-intel', 'macos-15'])
        self.assertIn("runner.os != 'Linux' && inputs.scope == 'full'", text)
        self.assertIn('test --suite resources', text)
        self.assertEqual(len(matrices[2]), 5)
        self.assertEqual(sum(entry.get('profile') == 'msvc2019' for entry in matrices[2]), 1)
        self.assertIn("runner.os == 'Linux' && inputs.scope == 'full'", text)
        self.assertIn("if: inputs.scope == 'workspace-linux'", text)
        self.assertIn("Workspace references (Linux)", text)
        self.assertIn("Fatal Qt diagnostic", text)

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
