"""Install the exact sealed development .deb on a disposable GitHub runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

from installer_common import build_output, digest


def run(command, log, *, env=None, timeout=60, check=True):
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    with log.open('a', encoding='utf-8') as stream:
        stream.write(f"$ {' '.join(map(str, command))}\n{result.stdout}{result.stderr}\n")
    if check:
        result.check_returncode()
    return result


def verify(package, metadata, apt_sources, evidence):
    if (os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'
            or platform.machine() != 'x86_64' or platform.freedesktop_os_release().get('VERSION_ID') != '22.04'
            or platform.freedesktop_os_release().get('ID') != 'ubuntu' or os.geteuid() == 0):
        raise ValueError('Expected a disposable GitHub-hosted Ubuntu 22.04 x86_64 runner')
    runtime_id = metadata['runtimeId']
    if not metadata['ok'] or metadata['sha256'] != digest(package) or not re.fullmatch(r'[a-f0-9]{40}-[a-f0-9]{12}', runtime_id):
        raise ValueError('Expected the sealed package and its original build report')
    build = metadata['buildIdentity']['buildVersion']
    if runtime_id.split('-')[0] != build['revision'] or build['revision'] != os.environ.get('GITHUB_SHA') or build['dirty']:
        raise ValueError('Package build does not match the clean workflow commit')

    evidence.mkdir(parents=True)
    report = {'ok': False, 'packageSha256': metadata['sha256'], 'buildIdentity': metadata['buildIdentity'],
              'scope': 'Ubuntu apt/dpkg and installed X11 launcher on a disposable runner', 'checks': []}
    log = evidence / 'package.log'
    launcher = Path('/usr/bin/xdnote')
    desktop_file = Path('/usr/share/applications/xdnote.desktop')
    runtime = Path('/opt/xdnote/versions') / runtime_id
    binary = runtime / 'bin/xdnote'
    owned = [launcher, desktop_file, runtime, Path('/usr/libexec/xdnote-update-helper'),
             Path('/usr/share/polkit-1/actions/org.xdnote.update.policy')]
    apt = ['/usr/bin/sudo', '-n', '/usr/bin/apt-get', '-o', f'Dir::Etc::sourcelist={apt_sources}',
           '-o', 'Dir::Etc::sourceparts=-', '-o', 'Acquire::Retries=1',
           '-o', 'Acquire::http::Timeout=20', '-o', 'Acquire::https::Timeout=20', '-y', '--no-remove']

    def query():
        return run(['/usr/bin/dpkg-query', '-W', '-f=${db:Status-Status}\t${Version}\t${Architecture}', 'xdnote'],
                   log, check=False)

    try:
        if query().returncode == 0 or any(p.exists() or p.is_symlink() for p in owned):
            raise ValueError('Runner already has xdnote state')
        if any(p.exists() or p.is_symlink() for p in (Path('/opt/xdnote'), Path('/run/xdnote'))):
            raise ValueError('Runner already has xdnote directories')
        with tempfile.TemporaryDirectory(prefix='profile-', dir=evidence) as directory:
            config = Path(directory)
            fixture = config / '自建文档.md'
            fixture.write_text('# 系统包验证\n', encoding='utf-8')
            before = digest(fixture)
            env = os.environ.copy()
            env.update(HOME=directory, XDG_CONFIG_HOME=str(config / 'config'), XDG_DATA_HOME=str(config / 'data'),
                       XDG_CACHE_HOME=str(config / 'cache'), QT_QPA_PLATFORM='xcb')
            try:
                run([*apt, '--no-install-recommends', 'install', str(package)], log, timeout=300)
                status = query()
                if status.returncode or status.stdout != f"installed\t{metadata['version']}\tamd64":
                    raise ValueError('dpkg status does not match the sealed package')
                for path in [launcher, desktop_file, binary, *owned[3:]]:
                    owner = run(['/usr/bin/dpkg-query', '-S', str(path)], log).stdout.strip()
                    if owner not in (f'xdnote: {path}', f'xdnote:amd64: {path}'):
                        raise ValueError(f'Unexpected package owner for {path}')
                if run(['/usr/bin/dpkg', '--verify', 'xdnote'], log).stdout.strip():
                    raise ValueError('dpkg found modified or missing package files')
                if not digest(binary).startswith(runtime_id.split('-')[1]):
                    raise ValueError('Installed binary bytes differ from package identity')
                report['checks'].append('apt installed expected package and dpkg confirmed ownership')
                run(['/usr/bin/desktop-file-validate', desktop_file], log)
                if 'Exec=/usr/bin/xdnote -- %f' not in desktop_file.read_text().splitlines():
                    raise ValueError('Desktop entry does not use the installed launcher')
                launched = json.loads(run([launcher, '--version-json'], log, env=env).stdout)
                if any(launched.get(key) != build[key] for key in ('product', 'version', 'build', 'revision', 'channel', 'dirty')):
                    raise ValueError('Installed launcher reported a different build')
                run(['/usr/bin/xvfb-run', '-a', launcher, '--help'], log, env=env)
                report['checks'].append('installed launcher returned the expected build and initialized native X11 Qt')
            finally:
                if query().returncode == 0:
                    run(['/usr/bin/sudo', '-n', '/usr/bin/dpkg', '--purge', 'xdnote'], log, timeout=150)
            if query().returncode == 0 or any(p.exists() or p.is_symlink() for p in owned):
                raise ValueError('Package or owned installed files survived purge')
            if Path('/run/xdnote/installing').exists() or digest(fixture) != before:
                raise ValueError('Purge retained the maintenance marker or modified the user fixture')
            report['checks'].append('dpkg purge removed owned installation and preserved the user fixture')
            report['ok'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('package', 'metadata', 'apt-sources', 'evidence'):
        parser.add_argument('--' + argument, type=Path, required=True)
    args = parser.parse_args()
    verify(build_output(args.package), json.loads(build_output(args.metadata).read_bytes()),
           build_output(args.apt_sources), build_output(args.evidence))
