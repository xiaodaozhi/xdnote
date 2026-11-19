"""Verify a real Markdown content-type default on a disposable macOS runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
from product_deployment_unix import inventory
from installer_common import build_output

def verify(root, audit, output):
    if (platform.system() != 'Darwin' or os.geteuid() == 0
            or os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'):
        raise ValueError('Use an unprivileged disposable GitHub-hosted macOS account')
    identity = audit['buildIdentity']
    build = identity['buildVersion']
    if not audit['ok'] or build['dirty'] or build['revision'] != os.environ.get('GITHUB_SHA') or inventory(root) != audit['files']:
        raise ValueError('Use the exact clean sealed development bundle')
    output.mkdir()
    app = root / 'xdnote.app'
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', app], check=True, timeout=30)
    probe = output / 'markdown-default'
    subprocess.run(['/usr/bin/xcrun', 'swiftc', Path(__file__).with_suffix('.swift'),
                    '-module-cache-path', output / 'module-cache', '-o', probe], check=True, timeout=180)
    result = subprocess.run([probe, app, output], timeout=100, check=False)
    receipt = json.loads((output / 'result.json').read_bytes())
    receipt.update(buildIdentity=identity, unchangedProduct=inventory(root) == audit['files'])
    (output / 'report.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n')
    if result.returncode or not receipt['ok'] or not receipt['unchangedProduct']:
        raise RuntimeError(receipt.get('error', 'Native Markdown default acceptance failed'))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'audit', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    verify(build_output(args.root), json.loads(build_output(args.audit).read_bytes()), build_output(args.output))
