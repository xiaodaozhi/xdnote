"""Exercise native polkit/PAM in an isolated local login on a hosted Ubuntu runner."""
import argparse
from contextlib import nullcontext
import fcntl
import json
import os
from pathlib import Path
import platform
import pty
import pwd
import re
import secrets
import select
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import termios
import time

ACTION = 'org.xdnote.update'
POLICY = Path('/usr/share/polkit-1/actions/org.xdnote.update.policy')
SOURCE = Path(__file__).resolve().parents[1]
HELPER = '/usr/libexec/xdnote-update-helper'
LAUNCHER = '/usr/bin/xdnote'
PACKAGE = 'xdnote-0.1.0-alpha.2-ubuntu22.04-x86_64.deb'


def sudo(*args, timeout=30, **options):
    return subprocess.run(['/usr/bin/sudo', '-n', *map(str, args)], check=True, timeout=timeout, **options)


def session_properties():
    process = subprocess.run(['/usr/bin/loginctl', 'show-session', os.environ.get('XDG_SESSION_ID', 'self'),
                              '-p', 'Active', '-p', 'Remote', '-p', 'Type', '-p', 'State', '-p', 'Seat'],
                             check=True, capture_output=True, text=True, timeout=10)
    values = dict(line.split('=', 1) for line in process.stdout.splitlines() if '=' in line)
    if values.get('Active') != 'yes' or values.get('Remote') != 'no' or not values.get('Seat'):
        raise RuntimeError('Expected a local active login: ' + repr(values))
    return values


def subject(pid):
    # Linux stat field 22 counts the process start in clock ticks.
    start = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()[19]
    return f'{pid},{start}'


def wait_for(path, process, timeout=20):
    deadline = time.monotonic() + timeout
    while not path.is_file():
        if process.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError(f'No {path.name} from the owned process (exit {process.poll()})')
        time.sleep(.05)
    return path


def authorize_at_prompt(command, password, *, cancel=False, installed_helper=False, after_grant=None):
    from pexpect import EOF, TIMEOUT
    from pexpect.fdpexpect import fdspawn

    request = None
    if installed_helper:
        # Register a process agent before the fixed helper can call pkexec.
        request = subprocess.Popen(['/bin/sh', '-c', 'kill -STOP $$; exec "$@"', 'xdnote-polkit', *command],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and request.poll() is None:
            if re.search(r'^State:\s+T\b', Path(f'/proc/{request.pid}/status').read_text(), re.MULTILINE):
                break
            time.sleep(.03)
        else:
            if request.poll() is None:
                request.kill()
                request.wait(timeout=5)
            raise RuntimeError('The fixed authorization caller did not reach the agent registration barrier')
    master, slave = pty.openpty()
    notified, notify = os.pipe()
    descriptor = subject(request.pid if request else os.getpid())

    def terminal():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    agent = subprocess.Popen(['/usr/bin/pkttyagent', '--process', descriptor, '--notify-fd', str(notify)],
                             stdin=slave, stdout=slave, stderr=slave, pass_fds=(notify,),
                             preexec_fn=terminal, env=dict(os.environ, LC_ALL='C'))
    os.close(slave)
    os.close(notify)
    terminal_output = fdspawn(master, encoding='utf-8', codec_errors='replace', timeout=15)
    authorized_install = False
    try:
        if not select.select([notified], [], [], 10)[0] or os.read(notified, 1) or agent.poll() is not None:
            raise RuntimeError('Native polkit agent did not register for this process')
        if request is not None:
            request.send_signal(signal.SIGCONT)
        else:
            request = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
        patterns = [r'Password:\s*', r'Choose identity[^\r\n]*:', TIMEOUT, EOF]
        position = terminal_output.expect(patterns)
        if position in (0, 1) and f'AUTHENTICATING FOR {ACTION} ' not in terminal_output.before:
            raise RuntimeError('The native prompt belongs to a different polkit action')
        if position == 1 and cancel:
            terminal_output.sendline('')
            terminal_output.expect(r'AUTHENTICATION CANCEL[LE]*D')
            return request.wait(timeout=20)
        if position == 1:
            account = os.environ['USER']
            match = next((re.match(r'\s*(\d+)\.\s+(.*)', line) for line in terminal_output.before.splitlines()
                          if re.search(r'(?<![\w-])' + re.escape(account) + r'(?![\w-])', line)), None)
            if not match:
                raise RuntimeError('The disposable administrator was absent from the polkit identity chooser')
            terminal_output.sendline(match.group(1))
            position = terminal_output.expect(patterns)
        if position != 0:
            status = request.poll()
            message = request.stderr.read(256).decode(errors='replace').strip() if status is not None and request.stderr else ''
            banner = terminal_output.before.strip()[-350:]
            raise RuntimeError(f'No administrator password prompt (agent state {position}, request exit {status}): {message} {banner}')
        if cancel:
            terminal_output.send('\x03')
        else:
            deadline = time.monotonic() + 2
            while termios.tcgetattr(master)[3] & termios.ECHO and time.monotonic() < deadline:
                time.sleep(.03)
            if termios.tcgetattr(master)[3] & termios.ECHO:
                raise RuntimeError('The authorization agent did not disable password echo')
            terminal_output.sendline(password)
            if after_grant is not None:
                authorized_install = True
                after_grant(request)
        return request.wait(timeout=20)
    finally:
        os.close(notified)
        if request is not None and request.poll() is None:
            if authorized_install:
                request.wait(timeout=320)
            else:
                request.kill()
                request.wait(timeout=5)
        if request is not None and request.stderr is not None:
            request.stderr.close()
        if agent.poll() is None:
            agent.terminate()
        agent.wait(timeout=5)
        terminal_output.close()


def write_control(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value))
    temporary.replace(path)


def run_installed_helper(password, report, authorize):
    home = Path.home()
    config = json.loads((home / 'fixture.json').read_bytes())
    old = config['old']
    new = config['new']
    install_root = Path(Path(LAUNCHER).read_text().split('"')[1]).parent.parent
    parent_env = os.environ.copy()
    desktop = report.get('graphicalAgent')
    if desktop:
        parent_env.pop('DISPLAY', None)
        parent_env.pop('XAUTHORITY', None)
        parent_env.update(WAYLAND_DISPLAY=desktop['waylandSocket'], XDG_SESSION_TYPE='wayland', XDG_CURRENT_DESKTOP='GNOME')

    def installed_identity():
        return json.loads(subprocess.check_output([LAUNCHER, '--version-json'], timeout=10))

    if installed_identity() != old:
        raise RuntimeError('The installed old identity does not match the signed fixture')
    for cancel in (True, False):
        mode = 'cancel' if cancel else 'install'
        running = home / ('parent-' + mode)
        running.mkdir(mode=0o700)
        job = home / ('job-' + mode)
        job.mkdir(mode=0o700)
        for name in (PACKAGE, 'release.json', 'release.json.sig'):
            shutil.copyfile(home / name, job / name)
        parent = subprocess.Popen([LAUNCHER, '--wait', running], env=parent_env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        transaction = secrets.token_hex(16)
        control = {'transaction': transaction}
        proceeding = False

        def finish_after_grant(helper):
            nonlocal proceeding
            try:
                ready = json.loads(wait_for(job / 'ready.json', helper).read_bytes())
                if ready != control or parent.poll() is not None:
                    raise RuntimeError('The root helper did not await the running old image')
                write_control(job / 'proceed.json', control)
                proceeding = True
                time.sleep(.35)
                if helper.poll() is not None or installed_identity() != old:
                    raise RuntimeError('The package changed before the running image exited')
                (running / 'stop-parent').touch()
                parent.wait(timeout=10)
                helper.wait(timeout=150)
            finally:
                if not proceeding:
                    write_control(job / 'cancel.json', control)
                (running / 'stop-parent').touch()

        try:
            wait_for(running / 'parent-ready', parent)
            write_control(job / 'request.json', {'schema_version': 1, 'transaction': transaction,
                          'parent_pid': parent.pid, 'current_version': old['version'], 'current_build': old['build'],
                          'install_root': str(install_root), 'package_file': str(job / PACKAGE),
                          'manifest_file': str(job / 'release.json'), 'signature_file': str(job / 'release.json.sig')})
            status = authorize([HELPER, '--authorize', job / 'request.json'], password, cancel=cancel,
                               installed_helper=True, after_grant=None if cancel else finish_after_grant)
            receipt = json.loads((job / 'result.json').read_bytes())
            if receipt.get('transaction') != transaction:
                raise RuntimeError('The receipt refers to a different authorization request')
            if cancel:
                cancel_codes = ('authorization_cancelled',) if desktop else ('authorization_cancelled', 'authorization_failed')
                if not status or receipt.get('error') not in cancel_codes or (job / 'ready.json').exists():
                    raise RuntimeError('The native cancellation did not leave the update unauthorized: ' + repr(receipt))
                if installed_identity() != old or (home / 'restarted.json').exists():
                    raise RuntimeError('Cancelling authorization changed the installed package')
            elif status or receipt.get('error') or installed_identity() != new:
                raise RuntimeError('The authorized install did not return the expected installed identity: ' + repr(receipt))
            else:
                deadline = time.monotonic() + 10
                restart = home / 'restarted.json'
                while not restart.is_file() and time.monotonic() < deadline:
                    time.sleep(.05)
                if json.loads(restart.read_bytes()) != new:
                    raise RuntimeError('The installed fixture restarted with an unexpected identity')
                if desktop:
                    restart_socket = home / 'restarted-wayland.txt'
                    if not restart_socket.is_file() or restart_socket.read_text() != desktop['waylandSocket']:
                        raise RuntimeError('The restarted installation did not reconnect to its original Wayland socket')
                    report['restartWaylandDisplay'] = restart_socket.read_text()
            report[mode] = {'receipt': receipt, 'exitStatus': status, 'identity': installed_identity()}
        finally:
            if not proceeding:
                write_control(job / 'cancel.json', control)
            (running / 'stop-parent').touch()
            parent.wait(timeout=10)


def run_worker(installed_update, graphical):
    worker = Path(__file__).resolve().parent
    if not worker.name.startswith('xdnote-polkit-') or Path.home() != worker / 'home' or os.getuid() == 0:
        raise RuntimeError('Run the authorization worker in the isolated virtual-console account')
    os.umask(0o077)
    path = Path.home() / 'password'
    password = path.read_text()
    path.unlink()
    report = {'ok': False}
    try:
        report['session'] = session_properties()
        session = nullcontext(authorize_at_prompt)
        if graphical:
            from polkit_gnome_session import graphical_session
            session = graphical_session(report)
        with session as authorize:
            if installed_update:
                run_installed_helper(password, report, authorize)
                report['ok'] = True
                return
            check = ['/usr/bin/pkcheck', '--action-id', ACTION, '--process', subject(os.getpid()) + ',' + str(os.getuid())]
            baseline = subprocess.run(check, capture_output=True, timeout=10)
            report['withoutInteraction'] = baseline.returncode
            if baseline.returncode == 0:
                raise RuntimeError('The action unexpectedly allowed an unauthenticated administrator')
            report['cancelStatus'] = authorize([*check, '--allow-user-interaction'], password, cancel=True)
            report['grantStatus'] = authorize([*check, '--allow-user-interaction'], password)
            report['ok'] = report['cancelStatus'] != 0 and report['grantStatus'] == 0
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        report['ok'] = False
        report['error'] = str(error)
    finally:
        (Path.home() / 'report.json').write_text(json.dumps(report))


def install_fixture(home, username, output):
    from check_update_helper import Fixture
    from installer_common import digest
    work = output / 'native-fixture'
    work.mkdir()
    fixture = Fixture(work, None, restart_receipt=home / 'restarted.json')
    if fixture.seed.exists():
        raise RuntimeError('A private signing seed was left in the fixture')
    files = {PACKAGE: fixture.packages['new'],
             'release.json': fixture.manifests['new'][0], 'release.json.sig': fixture.manifests['new'][1]}
    identities = work / 'fixture-identities.json'
    identities.write_text(json.dumps(fixture.identities))
    for name, path in {**files, 'fixture.json': identities}.items():
        sudo('/usr/bin/install', '-o', username, '-m', '0600', path, home / name)
    sudo('/usr/bin/apt-get', '-y', '--no-remove', '--no-install-recommends', 'install', fixture.packages['old'],
         capture_output=True, timeout=180)
    if POLICY.read_bytes() != (SOURCE / 'apps/desktop/packaging/linux/org.xdnote.update.policy').read_bytes():
        raise RuntimeError('The installed package did not carry the original polkit policy')
    return {'sourceRevision': fixture.revision, 'oldPackageSha256': digest(fixture.packages['old']),
            'newPackageSha256': digest(fixture.packages['new']), 'installedPolicySha256': digest(POLICY)}


def run_probe(output, installed_update, graphical):
    if (platform.system() != 'Linux' or os.getuid() == 0 or os.environ.get('GITHUB_ACTIONS') != 'true'
            or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'):
        raise ValueError('An unprivileged disposable GitHub-hosted Linux runner is required')
    if any(path.exists() for path in (POLICY, Path(HELPER), Path(LAUNCHER), Path('/opt/xdnote'))):
        raise ValueError('Preserve an existing policy or installation')
    if not Path('/dev/tty0').is_char_device():
        raise RuntimeError('The hosted Linux kernel exposes no virtual console')
    output.mkdir(parents=True)
    username = 'xdn-polkit-' + os.environ['GITHUB_RUN_ID'][-12:]
    password = secrets.token_hex(20)
    with tempfile.TemporaryDirectory(prefix='xdnote-polkit-', ignore_cleanup_errors=True) as directory:
        root = Path(directory)
        root.chmod(0o755)
        home = root / 'home'
        candidate = root / 'worker.py'
        candidate.write_bytes(Path(__file__).read_bytes())
        profile = root / 'login'
        worker = [sys.executable, str(candidate), '--inside-login']
        if graphical:
            shutil.copyfile(SOURCE / 'scripts/polkit_gnome_session.py', root / 'polkit_gnome_session.py')
            worker = ['/usr/bin/xvfb-run', '-a', '-s', '-screen 0 1280x900x24', '/usr/bin/dbus-run-session', '--',
                      '/usr/bin/python3', str(candidate), '--inside-login', '--graphical']
        if installed_update:
            worker.append('--installed-update')
        profile.write_text('exec ' + shlex.join(worker) +
                           ' >"$HOME/console.log" 2>&1\n')
        owned_user = False
        installed_policy = False
        candidate_receipt = {}
        opt_mode = None
        try:
            try:
                sudo('/usr/sbin/useradd', '--no-create-home', '--no-log-init', '--home-dir', home, '--shell', '/bin/bash',
                     '--groups', 'sudo', username, timeout=90)
            finally:
                try:
                    owned_user = Path(pwd.getpwnam(username).pw_dir) == home
                except KeyError:
                    pass
            sudo('/usr/bin/install', '-d', '-o', username, '-m', '0700', home)
            sudo('/usr/sbin/chpasswd', input=(username + ':' + password + '\n').encode())
            sudo('/usr/bin/install', '-o', username, '-m', '0600', profile, home / '.bash_profile')
            private = root / 'secret'
            with private.open('x') as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(password)
            sudo('/usr/bin/install', '-o', username, '-m', '0600', private, home / 'password')
            private.unlink()
            if installed_update:
                opt = Path('/opt')
                if opt.is_symlink() or opt.stat().st_uid != 0:
                    raise RuntimeError('The hosted /opt directory has an unexpected owner')
                opt_mode = stat.S_IMODE(opt.stat().st_mode)
                sudo('/usr/bin/chmod', '0755', opt)
                candidate_receipt = install_fixture(home, username, output)
            else:
                sudo('/usr/bin/install', '-m', '0644',
                     SOURCE / 'apps/desktop/packaging/linux/org.xdnote.update.policy', POLICY)
                installed_policy = True
            login = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/openvt', '-c', '12', '-s', '-w', '--',
                                    '/bin/login', '-f', '--', username],
                                   capture_output=True, timeout=410 if installed_update else 115)
            receipt = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/cat', home / 'report.json'],
                                     capture_output=True, timeout=10)
            console_log = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/cat', home / 'console.log'],
                                         capture_output=True, timeout=10)
            console = {'exit': login.returncode, 'warning': login.stderr.decode(errors='replace')[-500:],
                       'workerOutput': console_log.stdout.decode(errors='replace')[-1500:]}
            (output / 'console.json').write_text(json.dumps(console, indent=2) + '\n')
            if graphical:
                for name in ('gnome-shell.log', 'prompt-cancel.xwd', 'prompt-grant.xwd', 'prompt-diagnostic.xwd'):
                    picture = subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/cat', home / name],
                                             capture_output=True, timeout=10)
                    if picture.returncode == 0:
                        (output / name).write_bytes(picture.stdout)
            if receipt.returncode:
                raise RuntimeError('The console login did not return a receipt: ' + repr(console))
            result = json.loads(receipt.stdout)
            result.update(candidate_receipt, virtualConsole=12)
            (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
            if not result['ok']:
                raise RuntimeError(result.get('error', 'Unexpected polkit authorization outcome'))
        finally:
            if installed_update and Path(HELPER).exists():
                sudo('/usr/bin/dpkg', '--purge', 'xdnote', capture_output=True, timeout=120)
            if opt_mode is not None:
                sudo('/usr/bin/chmod', format(opt_mode, 'o'), '/opt')
            if installed_policy:
                sudo('/usr/bin/rm', '--', POLICY)
            if owned_user:
                subprocess.run(['/usr/bin/sudo', '-n', '/usr/bin/loginctl', 'terminate-user', username],
                               capture_output=True, timeout=10)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    result = subprocess.run(['/usr/bin/sudo', '-n', '/usr/sbin/userdel', '--remove', username],
                                            capture_output=True, timeout=10)
                    if result.returncode == 0:
                        break
                    time.sleep(.25)
                else:
                    raise RuntimeError('The disposable administrator could not be removed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside-login', action='store_true')
    parser.add_argument('--installed-update', action='store_true')
    parser.add_argument('--graphical', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.inside_login:
        run_worker(args.installed_update, args.graphical)
    else:
        from installer_common import build_output
        run_probe(build_output(args.output), args.installed_update, args.graphical)
