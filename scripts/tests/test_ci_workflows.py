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
        self.assertIn("options: [full, workspace-linux, resources-unix, resources-macos, resources-linux, preview-unix, background-unix, ui-unix, deployment-unix, deployment-linux, deployment-macos, scroll-unix, settings-unix, tools-unix, tools-macos, updates, sparkle-macos, installer-windows, update-helpers, update-products]", text)
        self.assertIn("inputs.scope == 'updates'", text)
        self.assertIn("test --suite updates", text)
        self.assertIn("inputs.scope == 'deployment-macos'", text)
        self.assertIn("if: inputs.scope != 'installer-windows'", text)
        self.assertIn("if: inputs.scope == 'installer-windows'", text)
        self.assertIn("uses: ./.github/workflows/windows-installer.yml", text)
        self.assertIn("(inputs.scope == 'workspace-linux' || inputs.scope == 'resources-linux' || inputs.scope == 'deployment-linux')", text)
        self.assertIn("Workspace resources - Linux only", text)
        self.assertIn("failure() && runner.os == 'Linux' && inputs.scope == 'workspace-linux'", text)
        self.assertIn("default: full", text)
        matrices = [json.loads(value) for value in re.findall(r"'(\[\{[^\n]+?\}\])'", text)]
        self.assertEqual(len(matrices), 5)
        self.assertEqual(len(matrices.pop(0)), 3)
        self.assertEqual([entry['os'] for entry in matrices[0]], ['ubuntu-22.04'])
        self.assertEqual([entry['os'] for entry in matrices[1]], ['macos-15'])
        self.assertEqual([entry['os'] for entry in matrices[2]], ['ubuntu-22.04', 'macos-15'])
        self.assertIn("runner.os != 'Linux' && inputs.scope == 'full'", text)
        self.assertIn('test --suite resources', text)
        self.assertEqual(len(matrices[3]), 4)
        self.assertEqual(sum(entry.get('profile') == 'msvc2019' for entry in matrices[3]), 1)
        self.assertIn("runner.os == 'Linux' && inputs.scope == 'full'", text)
        self.assertIn("if: inputs.scope == 'workspace-linux'", text)
        self.assertIn("Workspace references (Linux)", text)
        self.assertIn("Fatal Qt diagnostic", text)
        self.assertIn("if: inputs.scope == 'preview-unix'", text)
        self.assertIn("test --suite preview", text)
        self.assertIn("if: inputs.scope == 'background-unix'", text)
        self.assertIn("test --suite background", text)
        self.assertIn("if: inputs.scope == 'ui-unix'", text)
        self.assertIn("test --suite ui", text)
        self.assertIn("if: inputs.scope == 'settings-unix'", text)
        self.assertIn("test --suite settings", text)
        self.assertIn("if: startsWith(inputs.scope, 'tools-')", text)
        self.assertIn("inputs.scope == 'tools-macos'", text)
        self.assertIn("test --suite tools", text)
        self.assertIn("Document tools (Unix)", text)
        self.assertIn("if: inputs.scope == 'scroll-unix'", text)
        self.assertIn("test --suite scroll", text)
        self.assertIn("if: startsWith(inputs.scope, 'deployment-')", text)
        self.assertIn("python scripts/product_deployment_unix.py", text)
        self.assertIn("--audit build/cycle-112-deployment/staging-evidence/inventory.json", text)
        self.assertIn("installed-check-evidence/", text)
        self.assertIn("staging-evidence/", text)
        self.assertIn("patchelf", text)
        self.assertIn("xdnote_apt_archive=https://archive.ubuntu.com/ubuntu", text)
        self.assertIn("https://mirrors.kernel.org/ubuntu", text)
        self.assertIn("timeout 10s getent ahosts", text)
        self.assertIn("signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg", text)
        self.assertIn("timeout 180s sudo apt-get", text)
        self.assertIn("timeout 360s sudo apt-get", text)
        self.assertIn("Dir::Etc::sourceparts=-", text)

    def test_sparkle_admission_is_separate_from_desktop_and_release(self):
        text = (WORKFLOWS / 'desktop-admission.yml').read_text(encoding='utf-8')
        job = text.split('  sparkle:', 1)[1].split('  desktop:', 1)[0]
        self.assertIn("if: inputs.scope == 'sparkle-macos'", job)
        self.assertIn('os: [macos-15]', job)
        self.assertIn('python scripts/sparkle.py prepare --download', job)
        self.assertIn('python scripts/sparkle.py build', job)
        self.assertIn('python scripts/check_sparkle.py', job)
        self.assertIn('needs.sparkle.result', text)
        self.assertIn("if: inputs.scope != 'installer-windows' && inputs.scope != 'sparkle-macos'", text)
        for forbidden in ('prepare_qt', 'desktop.py', 'pip install', 'secrets.', 'contents: write', 'gh release'):
            self.assertNotIn(forbidden, job)

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
        for name in ("desktop-admission.yml", "qt-document-spike.yml", "scintilla-buffer-spike.yml", "pulldown-markdown-spike.yml", "update-core.yml"):
            with self.subTest(workflow=name):
                self.assertEqual(events(WORKFLOWS / name), {"workflow_dispatch"})

    def test_update_core_is_independent_of_desktop_and_release(self):
        text = (WORKFLOWS / 'update-core.yml').read_text(encoding='utf-8')
        self.assertIn('runs-on: ubuntu-22.04', text)
        self.assertIn('contents: read', text)
        self.assertIn('build/cargo-home', text)
        self.assertIn('cargo fetch --locked', text)
        self.assertIn('cargo test --locked --offline --workspace', text)
        self.assertIn('cargo clippy --locked --offline --workspace --all-targets -- -D warnings', text)
        self.assertIn('python scripts/check_third_party_licenses.py', text)
        for forbidden in ('desktop.py', 'prepare_qt', 'matrix:', 'contents: write', 'secrets.', 'gh release'):
            self.assertNotIn(forbidden, text)

    def test_native_helper_uses_disposable_explicit_platforms_without_publishing(self):
        text = (WORKFLOWS / 'update-helper.yml').read_text(encoding='utf-8')
        self.assertEqual(events(WORKFLOWS / 'update-helper.yml'), {'workflow_dispatch', 'workflow_call'})
        dispatcher = (WORKFLOWS / 'desktop-admission.yml').read_text(encoding='utf-8')
        self.assertIn("if: inputs.scope == 'update-helpers'", dispatcher)
        self.assertIn('uses: ./.github/workflows/update-helper.yml', dispatcher)
        self.assertIn('needs.update-helpers.result', dispatcher)
        self.assertIn("if: inputs.scope == 'update-products'", dispatcher)
        self.assertIn('needs.update-products.result', dispatcher)
        self.assertIn('os: [ubuntu-22.04, windows-2022]', text)
        self.assertIn('contents: read', text)
        self.assertIn('persist-credentials: false', text)
        self.assertIn('scripts/check_update_helper.py', text)
        self.assertIn('scripts/check_native_product_update.py', text)
        self.assertIn('retention-days: 7', text)
        for forbidden in ('contents: write', 'secrets.', 'gh release', 'pull_request:', 'push:'):
            self.assertNotIn(forbidden, text)


if __name__ == "__main__":
    unittest.main()
