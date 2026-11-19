"""Install the exact sealed development .deb on a disposable GitHub runner."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import select
import signal
import subprocess
import tempfile
import time

from installer_common import build_output, digest


def run(command, log, *, env=None, timeout=60, check=True):
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    with log.open('a', encoding='utf-8') as stream:
        stream.write(f"$ {' '.join(map(str, command))}\n{result.stdout}{result.stderr}\n")
    if check:
        result.check_returncode()
    return result


def window_pid(fixture, binary, environment=None):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        tree = subprocess.run(['/usr/bin/xwininfo', '-root', '-tree'], env=environment,
                              capture_output=True, text=True, check=True, timeout=3).stdout
        for window in re.findall(r'^ +((?:0x)[0-9a-f]+) ', tree, re.MULTILINE):
            properties = subprocess.run(['/usr/bin/xprop', '-id', window, '-notype', '_NET_WM_NAME', '_NET_WM_PID'],
                                        env=environment, capture_output=True, text=True, timeout=3).stdout
            if fixture.name in properties:
                match = re.search(r'^_NET_WM_PID = (\d+)$', properties, re.MULTILINE)
                if match:
                    pid = int(match[1])
                    if Path(f'/proc/{pid}/exe').resolve(strict=True) != binary:
                        raise ValueError('The opened window is not owned by the installed product')
                    return pid
        time.sleep(0.2)
    raise ValueError(f'No installed X11 document window for {fixture.name}')


def desktop_entry_launch(desktop_file, binary, fixtures, env, log):
    """Use GIO to exercise the installed entry and MIME handler on a private X11 server."""
    with log.open('ab') as output:
        with subprocess.Popen(['/usr/bin/Xvfb', '-displayfd', '1', '-nolisten', 'tcp', '-screen', '0', '1280x960x24'],
                              stdout=subprocess.PIPE, stderr=output, text=True) as display:
            launchers = []
            try:
                if not select.select([display.stdout], [], [], 10)[0]:
                    raise ValueError('Isolated X11 server did not initialize')
                number = display.stdout.readline().strip()
                if not number.isdecimal():
                    raise ValueError('Isolated X11 server did not return a display number')
                environment = dict(env, DISPLAY=':' + number, LC_ALL='C.UTF-8',
                                   XDG_DATA_DIRS='/usr/local/share:/usr/share')
                mime = 'text/markdown'
                mime_config = Path(environment['XDG_CONFIG_HOME']) / 'mimeapps.list'
                run(['/usr/bin/gio', 'mime', mime, desktop_file.name], log, env=environment)
                default = run(['/usr/bin/gio', 'mime', mime], log, env=environment).stdout.splitlines()[0]
                if not default.endswith(': ' + desktop_file.name) or not mime_config.is_file():
                    raise ValueError('GIO did not save the Markdown handler in the private XDG configuration')
                pids = []
                for fixture, command in zip(fixtures, (
                        ['/usr/bin/gio', 'open'],
                        ['/usr/bin/gio', 'launch', desktop_file],
                        ['/usr/bin/gio', 'open'],
                ), strict=True):
                    content = run(['/usr/bin/gio', 'info', '-a', 'standard::content-type', fixture],
                                  log, env=environment).stdout
                    if f'standard::content-type: {mime}' not in map(str.strip, content.splitlines()):
                        raise ValueError('GIO did not identify the Markdown fixture')
                    launch = subprocess.Popen([*command, fixture],
                                              env=environment, stdout=output, stderr=output, start_new_session=True)
                    launchers.append(launch)
                    if launch.wait(timeout=8):
                        raise ValueError(f'GIO failed to activate the installed desktop entry for {fixture.name}')
                    pids.append(window_pid(fixture, binary, environment))
                if len(set(pids)) != 1:
                    raise ValueError('Warm launch created another process instead of forwarding to the running instance')
                return {'defaultHandler': desktop_file.name, 'mimeType': mime,
                        'defaultColdDocument': fixtures[0].name, 'explicitWarmDocument': fixtures[1].name,
                        'defaultWarmDocument': fixtures[2].name, 'sameProcess': True}
            finally:
                # Both process groups were created by this fixture on the private display.
                for launch in launchers:
                    try:
                        os.killpg(launch.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                display.terminate()
                display.wait(timeout=5)


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
            second_fixture = config / '运行中 转交.md'
            second_fixture.write_text('# 系统桌面转交验证\n', encoding='utf-8')
            third_fixture = config / '默认应用 转交.md'
            third_fixture.write_text('# 默认应用运行中转交\n', encoding='utf-8')
            fixtures = (fixture, second_fixture, third_fixture)
            before = [digest(item) for item in fixtures]
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
                report['desktopLaunch'] = desktop_entry_launch(desktop_file, binary, fixtures, env, log)
                report['checks'].append('installed Markdown default handler and desktop entry opened documents in one process')
                with (tempfile.TemporaryDirectory(prefix='nautilus-', dir=evidence) as isolated,
                      tempfile.TemporaryDirectory(prefix='xdnote-runtime-') as runtime_dir):
                    home = Path(isolated)
                    files = home / 'File manager acceptance'
                    files.mkdir()
                    samples = [files / fixture.name for fixture in fixtures]
                    for source, target in zip(fixtures, samples, strict=True):
                        target.write_bytes(source.read_bytes())
                    desktop_env = dict(env, HOME=isolated, XDG_RUNTIME_DIR=runtime_dir,
                                       XDG_CONFIG_HOME=str(home / 'config'), XDG_DATA_HOME=str(home / 'data'),
                                       XDG_CACHE_HOME=str(home / 'cache'), XDG_DATA_DIRS='/usr/local/share:/usr/share',
                                       GDK_BACKEND='x11', GSETTINGS_BACKEND='memory', NO_AT_BRIDGE='0',
                                       GTK_MODULES='atk-bridge', LC_ALL='C.UTF-8')
                    command = ['/usr/bin/dbus-run-session', '--', '/usr/bin/xvfb-run', '-a',
                               '/usr/bin/python3', Path(__file__).with_name('check_nautilus_open.py'),
                               binary, evidence, *samples]
                    run(command, log, env=desktop_env, timeout=150)
                    if [digest(item) for item in samples] != before:
                        raise ValueError('File-manager acceptance modified a user fixture')
                report['nautilus'] = json.loads((evidence / 'nautilus.json').read_bytes())
                report['checks'].append('Nautilus opened a file once, changed the Markdown default, and used that default')
            finally:
                if query().returncode == 0:
                    run(['/usr/bin/sudo', '-n', '/usr/bin/dpkg', '--purge', 'xdnote'], log, timeout=150)
            if query().returncode == 0 or any(p.exists() or p.is_symlink() for p in owned):
                raise ValueError('Package or owned installed files survived purge')
            if Path('/run/xdnote/installing').exists() or [digest(item) for item in fixtures] != before:
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
