"""Drive one Sparkle upgrade through the real GitHub release on hosted arm64."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import time

import desktop
import sparkle
from check_native_update import OWNER, TEMPLATES, checked, dump, replace
from installer_common import ROOT, build_output, digest
from mac_public_bootstrap import source_tree


def running(binary):
    processes = []
    output = subprocess.check_output(['/bin/ps', '-axo', 'pid=,command='], text=True)
    for line in output.splitlines():
        parts = line.strip().split(maxsplit=1)
        if len(parts) == 2 and (parts[1] == str(binary) or parts[1].startswith(str(binary) + ' ')):
            processes.append(int(parts[0]))
    return processes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('work-dir', 'qt-prefix', 'sealed-candidate', 'signed-attachments'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    if (platform.system(), platform.machine()) != ('Darwin', 'arm64') or any(os.environ.get(k) != v for k, v in {
        'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted', 'GITHUB_REPOSITORY': 'xiaodaozhi/xdnote',
        'GITHUB_REF_NAME': 'coding/155-rc3-public-update',
    }.items()):
        raise ValueError('Use the dedicated disposable hosted Apple Silicon runner')

    work = build_output(args.work_dir)
    work.mkdir(exist_ok=False)
    qt = desktop.checked_tool_path(args.qt_prefix, 'Qt SDK', sdk=True)
    sealed = build_output(args.sealed_candidate)
    attachments = build_output(args.signed_attachments)
    identity = json.loads((sealed / 'identity.json').read_bytes())
    expected = {k: v for k, v in identity.items() if k not in {'schemaVersion', 'configuration'}}
    environment = desktop.environment(None)
    environment['CARGO_NET_OFFLINE'] = 'true'
    rust = Path(checked(['rustup', 'which', '--toolchain', '1.99.0', 'rustc'], env=environment).stdout.decode().strip())
    environment['PATH'] = str(rust.parent) + os.pathsep + environment.get('PATH', '')
    trust = {name: digest(ROOT / 'tools' / name) for name in ('update-trust.json', 'sparkle-update-key.json')}
    application_dir = Path.home() / 'Applications'
    application_dir.mkdir(exist_ok=True)
    installed = application_dir / 'xdnote.app'
    binary = installed / 'Contents/MacOS/xdnote'
    marker = application_dir / '.fixture-allowed.json'
    if installed.exists() or installed.is_symlink() or marker.exists():
        raise ValueError('Do not overwrite a pre-existing application or fixture marker')
    result = {'ok': False, 'publicTransport': True, 'identity': identity,
              'manifestSha256': digest(attachments / 'release.json')}
    source = run = None
    try:
        source, run, revision = source_tree(work, qt, environment)
        if revision != identity['revision'] or any(digest(source/'tools'/name) != sha for name, sha in trust.items()):
            raise ValueError('The predecessor does not retain the frozen source and production roots')
        shutil.copytree(source/'build/old-product/xdnote.app', installed, symlinks=True)
        checked(['/usr/bin/codesign', '--verify', '--deep', '--strict', installed])
        before = digest(binary)
        expected_app = work / 'expected'
        archive = sealed/'assets/xdnote-0.1.0-rc.3-macos-arm64.app.zip'
        expected_app.mkdir()
        checked(['/usr/bin/ditto', '-x', '-k', archive, expected_app])
        target_hash = digest(expected_app/'xdnote.app/Contents/MacOS/xdnote')
        dump(marker, {'owner': OWNER, 'allowedExecutableSha256': [before]})
        checked([os.sys.executable, source/'scripts/desktop.py', 'run', '--profile', 'default', '--build-dir',
                 source/'build/desktop', '--qt-prefix', qt, '--fixture-bundle', installed], env=environment,
                log=work/'public-run.log')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if binary.is_file() and digest(binary) == target_hash and len(running(binary)) == 1:
                break
            time.sleep(.4)
        else:
            raise RuntimeError('Sparkle did not replace and restart the expected public release')
        checked(['/usr/bin/codesign', '--verify', '--deep', '--strict', installed])
        restart = json.loads(checked([binary, '--version-json']).stdout)
        if restart != expected or (run/'owned.md').read_bytes() != b'edited owned\n':
            raise ValueError('The installed identity or saved fixture document differs')
        if json.loads((run/'settings.json').read_bytes())['fixtureUnknown'] != 'retained':
            raise ValueError('The fixture setting was lost')
        result.update(ok=True, restartObserved=True, restartedIdentity=restart, documentPreserved=True,
                      unknownSettingPreserved=True, archiveSha256=digest(archive), deployedBinarySha256=target_hash,
                      publicFeed=json.loads((attachments/'release.json').read_bytes())['macos_appcast_url'])
    finally:
        if installed.is_dir() and marker.is_file():
            for pid in running(binary):
                os.kill(pid, signal.SIGTERM)
        if run is not None:
            for name in ('status.json', 'error.json', 'native-error.json'):
                if (run/name).is_file():
                    shutil.copy2(run/name, work/name)
        dump(work/'report.json', result)


if __name__ == '__main__':
    main()
