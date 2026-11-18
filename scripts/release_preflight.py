"""Read-only candidate metadata/authentication preflight; never signs or publishes.

Native package inspection, applicable license/source fulfillment and platform
acceptance remain separate gates even when authentication succeeds.
"""
import argparse
import json
from pathlib import Path
import subprocess
import desktop
import release_candidate
from installer_common import ROOT, build_output, digest


def preflight(identity, assets, verification, manifest=None, signature=None, verifier=None):
    if any(value is not None for value in [manifest, signature, verifier]) and not all(value is not None for value in [manifest, signature, verifier]):
        raise ValueError('Provide original manifest, signature and compiled verifier together')
    candidate = release_candidate.manifest(identity, assets, verification)
    result = {'ok': True, 'identity': identity, 'candidate': candidate, 'authenticated': False,
              'licenseReviewComplete': False, 'releaseReady': False, 'blockers': [],
              'scope': 'actual six-asset hash/identity metadata and optional compiled-root authentication; not native package or final delivery acceptance'}
    trust = ROOT / 'tools/update-trust.json'; sparkle = ROOT / 'tools/sparkle-update-key.json'
    result['trustInputs'] = {'updateRootSha256': digest(trust), 'sparklePublicKeySha256': digest(sparkle)}
    roots = json.loads(trust.read_bytes()).get('keys')
    if not roots: result['blockers'].append('production_update_root_missing')
    if not json.loads(sparkle.read_bytes()).get('publicKey'): result['blockers'].append('production_sparkle_public_key_missing')
    if manifest is not None:
        if manifest.stat().st_size > 256 * 1024 or json.loads(manifest.read_bytes()) != candidate: raise ValueError('Original manifest differs from verified candidate metadata')
        result['manifestSha256'] = digest(manifest)
    if signature is not None and signature.stat().st_size > 4096: raise ValueError('Signature metadata exceeds bound')
    if all(value is not None for value in [manifest, signature, verifier]):
        executable = desktop.checked_tool_path(verifier, 'Compiled public-root verifier')
        owner = desktop.repository_owner(executable)
        if not owner or not executable.is_relative_to(owner[0] / 'build'): raise ValueError('Use the project-owned verifier build')
        run = subprocess.run([str(executable), str(manifest), str(signature)], capture_output=True, timeout=30)
        if len(run.stdout) > 8192 or run.returncode not in (0, 2): raise ValueError('Verifier failed its bounded JSON contract')
        authentication = json.loads(run.stdout)
        if authentication.get('trustRootSha256') != digest(trust) or authentication.get('manifestSha256') != digest(manifest):
            raise ValueError('Verifier public root or manifest identity differs')
        if (type(authentication.get('authenticated')) is not bool or type(authentication.get('ok')) is not bool
                or authentication['authenticated'] != (run.returncode == 0) or authentication['ok'] != authentication['authenticated']
                or (authentication['authenticated'] and not roots)):
            raise ValueError('Contradictory verifier status')
        result['authenticated'] = authentication['authenticated']; result['authenticationError'] = authentication.get('error', '')
        result['verifierSha256'] = digest(executable)
    if not result['authenticated']: result['blockers'].append('original_manifest_not_authenticated')
    result['blockers'] += ['native_asset_and_install_acceptance', 'final_license_and_source_fulfillment', 'minimum_system_and_real_desktop_acceptance', 'specific_formal_release_authorization']
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['identity', 'assets', 'verification', 'output']: p.add_argument('--' + name, type=Path, required=True)
    for name in ['manifest', 'signature', 'verifier']: p.add_argument('--' + name, type=Path)
    p.add_argument('--require-authenticated', action='store_true')
    a = p.parse_args(); output = build_output(a.output)
    if output.exists(): p.error('Preserve prior preflight evidence')
    result = preflight(json.loads(a.identity.read_bytes()), build_output(a.assets), json.loads(a.verification.read_bytes()), a.manifest, a.signature, a.verifier)
    output.parent.mkdir(parents=True, exist_ok=True); output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': result['ok'], 'authenticated': result['authenticated'], 'releaseReady': False, 'blockers': result['blockers']}))
    if a.require_authenticated and not result['authenticated']: raise SystemExit(2)
