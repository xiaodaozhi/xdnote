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
    def test_store_candidate_uses_real_identity_and_encrypts_transfer(self):
        path = WORKFLOWS / 'store-candidate.yml'
        workflow = path.read_text(encoding='utf-8')
        self.assertEqual(events(path), {'workflow_dispatch'})
        self.assertIn("github.repository == 'xiaodaozhi/xdnote' && github.ref == 'refs/heads/main'", workflow)
        self.assertIn('ref: ${{ github.sha }}', workflow)
        self.assertIn('--release-identity build/store-candidate/release-input.json', workflow)
        self.assertIn('vars.XDNOTE_MSIX_IDENTITY_JSON', workflow)
        self.assertIn('scripts/check-store-wack.ps1', workflow)
        self.assertIn('XDNOTE_TEST_FONT_FILE:', workflow)
        self.assertLess(workflow.index('scripts/prepare_ci_font.py'), workflow.index('scripts/deployment_inventory.py'))
        artifact = workflow.split('actions/upload-artifact@', 1)[1]
        self.assertIn('build/store-candidate/candidate.msix.age', artifact)
        self.assertNotIn('build/store-candidate/candidate.msix\n', artifact)
        self.assertNotIn('build/store-candidate/msix-identity.json', artifact)
        for forbidden in ('contents: write', 'secrets.', 'gh release', 'sign-release'):
            self.assertNotIn(forbidden, workflow)

    def test_release_keys_are_scoped_to_the_approved_signing_step(self):
        path = WORKFLOWS / 'release-signing.yml'
        workflow = path.read_text(encoding='utf-8')
        self.assertEqual(events(path), {'workflow_dispatch', 'workflow_call'})
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertIn('.head_sha == $sha', workflow)
        self.assertIn('environments/release-signing', workflow)
        self.assertIn('required_reviewers', workflow)
        self.assertIn('contents: read', workflow)
        self.assertIn('actions: read', workflow)
        self.assertNotIn('contents: write', workflow)
        approval, verify = workflow.split('  verify:', 1)
        before, signing = approval.split('      - name: Sign the verified candidate', 1)
        self.assertIn('environment: release-signing', before)
        self.assertIn('cargo build --locked --offline', before)
        for secret in ('XDNOTE_UPDATE_SIGNING_KEY_HEX', 'XDNOTE_SPARKLE_SIGNING_KEY_HEX'):
            self.assertIn(f'secrets.{secret}', signing)
            self.assertNotIn(secret, before)
            self.assertNotIn(secret, verify)
        for job in (approval, verify):
            self.assertIn('artifact-ids: ${{ needs.candidate.outputs.artifact }}', job)
        self.assertIn('scripts/release_updates.py verify --revision', verify)
        self.assertNotIn('environment:', verify)

    def test_production_key_check_only_derives_and_compares_after_approval(self):
        path = WORKFLOWS / 'release-key-check.yml'
        workflow = path.read_text(encoding='utf-8')
        self.assertEqual(events(path), {'workflow_dispatch'})
        self.assertIn("github.repository == 'xiaodaozhi/xdnote' && github.ref == 'refs/heads/main'", workflow)
        self.assertIn('required_reviewers', workflow)
        preparation, checks = workflow.split('      - name: Check manifest key', 1)
        self.assertIn('needs: approval-policy', preparation)
        self.assertIn('environment: release-signing', preparation)
        self.assertIn('cargo build --locked --offline', preparation)
        self.assertNotIn('secrets.', preparation)
        manifest, sparkle = checks.split('      - name: Check Sparkle key', 1)
        self.assertEqual(re.findall(r'secrets\.(\w+)', manifest), ['XDNOTE_UPDATE_SIGNING_KEY_HEX'])
        self.assertEqual(re.findall(r'secrets\.(\w+)', sparkle), ['XDNOTE_SPARKLE_SIGNING_KEY_HEX'])
        self.assertIn('vars.XDNOTE_UPDATE_KEY_ID', manifest)
        self.assertIn("!cancelled() && steps.build.outcome == 'success'", sparkle)
        self.assertEqual(re.findall(r'(?m)^        run: (.*)$', checks), [
            'build/release-tool/release/xdnote-update-tool check-manifest-key',
            'build/release-tool/release/xdnote-update-tool check-sparkle-key',
        ])
        for forbidden in ('contents: write', 'gh release', 'upload-artifact', 'release_updates.py'):
            self.assertNotIn(forbidden, workflow)

    def test_network_flow_is_explicit_and_does_not_upload_sources_or_keys(self):
        path = WORKFLOWS / 'qt-network-flow.yml'; text = path.read_text()
        self.assertEqual(events(path), {'workflow_dispatch', 'workflow_call'})
        for expected in ['ubuntu-22.04', 'windows-2022', 'macos-26', 'contents: read',
                         'scripts/check_qt_network_flow.py', 'scripts/prepare_qt.py', 'retention-days: 7']:
            self.assertIn(expected, text)
        upload = text.split('Preserve bounded flow evidence', 1)[1]
        for forbidden in ['contents: write', 'secrets.', 'gh release', '*.pem', '*.dll', '*.tar.xz', '*.patch']:
            self.assertNotIn(forbidden, upload)

    def test_delivery_review_is_read_only_and_keeps_source_run_explicit(self):
        text = (WORKFLOWS / 'delivery-review.yml').read_text()
        self.assertEqual(events(WORKFLOWS / 'delivery-review.yml'), {'workflow_dispatch', 'workflow_call'})
        for required in ['actions: read', 'contents: read', 'evidence_run:', 'scripts/component_review.py', 'scripts/fetch_delivery_evidence.py']:
            self.assertIn(required, text)
        self.assertIn('--supplier-only', text)
        dispatcher = (WORKFLOWS / 'desktop-admission.yml').read_text()
        self.assertIn("inputs.scope == 'delivery-review' && needs.delivery-review.result", dispatcher)
        self.assertIn('evidence_run: ${{ inputs.evidence_run }}', dispatcher)
        for forbidden in ['contents: write', 'gh release', 'desktop.py build', 'cargo build', '*.dll', '*.tar.xz']:
            self.assertNotIn(forbidden, text)
    def test_targeted_workspace_scope_keeps_full_matrix_explicit(self):
        text = (WORKFLOWS / "desktop-admission.yml").read_text(encoding="utf-8")
        self.assertIn("options: [full, workspace-linux, resources-unix, resources-macos, resources-linux, preview-unix, background-unix, ui-unix, deployment-unix, deployment-linux, deployment-macos, scroll-unix, settings-unix, tools-unix, tools-macos, updates, updates-macos, sparkle-macos, sparkle-system-pac, installer-windows, update-helpers, update-products, qt-replacement, delivery-review, network-flow, reader-orca, ime-linux]", text)
        self.assertIn("inputs.scope == 'updates'", text)
        self.assertIn("test --suite updates", text)
        self.assertIn("if: startsWith(inputs.scope, 'updates')", text)
        self.assertIn("inputs.scope == 'updates-macos'", text)
        self.assertIn('options: [macos-15, macos-26]', text)
        self.assertIn("matrix.os == 'macos-15' && inputs.macos_runner || matrix.os", text)
        self.assertIn("inputs.scope == 'deployment-macos'", text)
        self.assertIn("if: inputs.scope != 'installer-windows'", text)
        self.assertIn("if: inputs.scope == 'installer-windows'", text)
        self.assertIn("uses: ./.github/workflows/windows-installer.yml", text)
        self.assertIn("(inputs.scope == 'workspace-linux' || inputs.scope == 'resources-linux' || inputs.scope == 'deployment-linux' || inputs.scope == 'reader-orca' || inputs.scope == 'ime-linux')", text)
        self.assertIn("if: always() && inputs.scope == 'reader-orca'", text)
        for backend in ('x11', 'wayland'):
            self.assertIn(f'bash scripts/reader-acceptance/orca-linux.sh {backend}', text)
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
        self.assertIn("if: inputs.scope == 'deployment-linux' && runner.environment == 'github-hosted'", text)
        self.assertIn("scripts/check_linux_package_install.py", text)
        self.assertIn("native-package-evidence/", text)
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
        self.assertIn("if: inputs.scope == 'sparkle-macos' || inputs.scope == 'sparkle-system-pac'", job)
        self.assertIn('runs-on: ${{ inputs.macos_runner }}', job)
        self.assertIn('}}-${{ inputs.macos_runner }}', job)
        self.assertIn('python scripts/sparkle.py prepare --download', job)
        self.assertIn('python scripts/sparkle.py build', job)
        self.assertIn('python scripts/check_sparkle.py', job)
        self.assertIn('if [ "$XDNOTE_SPARKLE_SCOPE" = sparkle-system-pac ]; then', job)
        self.assertIn('xdnote_probe_options+=(--system-pac)', job)
        self.assertIn('needs.sparkle.result', text)
        self.assertIn("if: inputs.scope != 'installer-windows' && inputs.scope != 'sparkle-macos' && inputs.scope != 'sparkle-system-pac'", text)
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
        self.assertIn('cargo clippy --locked --offline --workspace --all-targets --features xdnote-update/release-tool -- -D warnings', text)
        self.assertIn('python scripts/check_third_party_licenses.py', text)
        for forbidden in ('desktop.py', 'prepare_qt', 'matrix:', 'contents: write', 'secrets.', 'gh release'):
            self.assertNotIn(forbidden, text)

    def test_native_helper_uses_disposable_explicit_platforms_without_publishing(self):
        text = (WORKFLOWS / 'update-helper.yml').read_text(encoding='utf-8')
        self.assertIn("if: inputs.proxy_matrix && inputs.scope != 'product'", text)
        self.assertIn("inputs.proxy_matrix && '--proxy-matrix'", text)
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

    def test_qt_source_replacement_is_explicit_owned_and_evidence_only(self):
        path = WORKFLOWS / 'qt-replacement.yml'; text = path.read_text()
        self.assertEqual(events(path), {'workflow_dispatch', 'workflow_call'})
        for expected in ('ubuntu-22.04', 'windows-2022', 'macos-26', 'contents: read', 'persist-credentials: false',
                         'scripts/build_modified_qt_core.py', 'scripts/verify_qt_replacement.py', 'scripts/linkage_audit.py'):
            self.assertIn(expected, text)
        self.assertIn('uses: ./.github/workflows/qt-replacement.yml', (WORKFLOWS / 'desktop-admission.yml').read_text())
        upload = text.split('Preserve source and replacement evidence only', 1)[1]
        for forbidden in ('contents: write', 'secrets.', 'gh release', 'signing certificate'):
            self.assertNotIn(forbidden, text)
        for forbidden in ('*.dll', '*.so', '*.framework', '*.tar.xz', 'core/source/'):
            self.assertNotIn(forbidden, upload)


if __name__ == "__main__":
    unittest.main()
