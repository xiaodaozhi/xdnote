"""Exercise native polkit/PAM in an isolated local login on a hosted Ubuntu runner."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import pty
import re
import secrets
import select
import shlex
import subprocess
import sys
import tempfile
import termios
import time

from pexpect import EOF, TIMEOUT, spawn
from pexpect.fdpexpect import fdspawn

ACTION = 'org.xdnote.update'
POLICY = Path('/usr/share/polkit-1/actions/org.xdnote.update.policy')
SOURCE = Path(__file__).resolve().parents[1]


def sudo(*args, **options):
    return subprocess.run(['/usr/bin/sudo', '-n', *map(str, args)], check=True, timeout=30, **options)


def session_properties():
    process = subprocess.run(['/usr/bin/loginctl', 'show-session', os.environ.get('XDG_SESSION_ID', 'self'),
                              '-p', 'Active', '-p', 'Remote', '-p', 'Type', '-p', 'State'],
                             check=True, capture_output=True, text=True, timeout=10)
    values = dict(line.split('=', 1) for line in process.stdout.splitlines() if '=' in line)
    if values.get('Active') != 'yes' or values.get('Remote') != 'no':
        raise RuntimeError('Expected a local active login: ' + repr(values))
    return values


def subject():
    # Linux stat field 22 counts the process start in clock ticks.
    start = Path('/proc/self/stat').read_text().rsplit(') ', 1)[1].split()[19]
    return f'{os.getpid()},{start}'


def authorize_at_prompt(command, password, *, cancel=False):
    master, slave = pty.openpty()
    notified, notify = os.pipe()
    descriptor = subject()

    def terminal():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    agent = subprocess.Popen(['/usr/bin/pkttyagent', '--process', descriptor, '--notify-fd', str(notify)],
                             stdin=slave, stdout=slave, stderr=slave, pass_fds=(notify,),
                             preexec_fn=terminal, env=dict(os.environ, LC_ALL='C'))
    os.close(slave)
    os.close(notify)
    request = None
    terminal_output = fdspawn(master, encoding='utf-8', codec_errors='replace', timeout=15)
    try:
        if not select.select([notified], [], [], 10)[0] or os.read(notified, 1) or agent.poll() is not None:
            raise RuntimeError('Native polkit agent did not register for this process')
        request = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        patterns = [r'Password:\s*', r'Choose identity[^\r\n]*:', TIMEOUT, EOF]
        position = terminal_output.expect(patterns)
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
            raise RuntimeError(f'No administrator password prompt (agent state {position}, request exit {status})')
        if cancel:
            terminal_output.sendcontrol('c')
        else:
            deadline = time.monotonic() + 2
            while termios.tcgetattr(master)[3] & termios.ECHO and time.monotonic() < deadline:
                time.sleep(.03)
            if termios.tcgetattr(master)[3] & termios.ECHO:
                raise RuntimeError('The authorization agent did not disable password echo')
            terminal_output.sendline(password)
        return request.wait(timeout=20)
    finally:
        os.close(notified)
        if request is not None and request.poll() is None:
            request.terminate()
            request.wait(timeout=5)
        if agent.poll() is None:
            agent.terminate()
        agent.wait(timeout=5)
        terminal_output.close()


def run_worker():
    path = Path.home() / 'password'
    password = path.read_text()
    path.unlink()
    report = {'ok': False}
    try:
        report['session'] = session_properties()
        check = ['/usr/bin/pkcheck', '--action-id', ACTION, '--process', subject() + ',' + str(os.getuid())]
        baseline = subprocess.run(check, capture_output=True, timeout=10)
        report['withoutInteraction'] = baseline.returncode
        if baseline.returncode == 0:
            raise RuntimeError('The action unexpectedly allowed an unauthenticated administrator')
        report['cancelStatus'] = authorize_at_prompt([*check, '--allow-user-interaction'], password, cancel=True)
        report['grantStatus'] = authorize_at_prompt([*check, '--allow-user-interaction'], password)
        report['ok'] = report['cancelStatus'] != 0 and report['grantStatus'] == 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    print('XDNOTE_POLKIT_RESULT=' + json.dumps(report), flush=True)


def run_probe(output):
    if (platform.system() != 'Linux' or os.getuid() == 0 or os.environ.get('GITHUB_ACTIONS') != 'true'
            or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'):
        raise ValueError('An unprivileged disposable GitHub-hosted Linux runner is required')
    if POLICY.exists():
        raise ValueError('Preserve an existing policy or installation')
    output.mkdir()
    username = 'xdn-polkit-' + os.environ['GITHUB_RUN_ID'][-12:]
    password = secrets.token_hex(20)
    with tempfile.TemporaryDirectory(prefix='xdnote-polkit-') as directory:
        root = Path(directory)
        root.chmod(0o755)
        home = root / 'home'
        candidate = root / 'worker.py'
        candidate.write_bytes(Path(__file__).read_bytes())
        profile = root / 'login'
        profile.write_text('exec ' + shlex.join([sys.executable, str(candidate), '--inside-login']) + '\n')
        owned_user = False
        installed_policy = False
        try:
            sudo('/usr/sbin/useradd', '--create-home', '--home-dir', home, '--shell', '/bin/bash',
                 '--groups', 'sudo', username)
            owned_user = True
            sudo('/usr/sbin/chpasswd', input=(username + ':' + password + '\n').encode())
            sudo('/usr/bin/install', '-o', username, '-m', '0600', profile, home / '.bash_profile')
            private = root / 'secret'
            with private.open('x') as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(password)
            sudo('/usr/bin/install', '-o', username, '-m', '0600', private, home / 'password')
            private.unlink()
            sudo('/usr/bin/install', '-m', '0644',
                 SOURCE / 'apps/desktop/packaging/linux/org.xdnote.update.policy', POLICY)
            installed_policy = True
            login = spawn('/usr/bin/sudo', ['-n', '/bin/login', '-f', '--', username],
                          encoding='utf-8', timeout=75)
            try:
                state = login.expect([r'XDNOTE_POLKIT_RESULT=([^\r\n]+)', EOF, TIMEOUT])
                if state:
                    bounded = re.sub(r'[^\x20-\x7e\n]', '', login.before)[-900:]
                    raise RuntimeError('The disposable login did not return a receipt: ' + bounded)
                result = json.loads(login.match.group(1))
                (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
                if not result['ok']:
                    raise RuntimeError(result.get('error', 'Unexpected polkit authorization outcome'))
            finally:
                login.close(force=True)
        finally:
            if installed_policy:
                sudo('/usr/bin/rm', '--', POLICY)
            if owned_user:
                sudo('/usr/sbin/userdel', '--remove', username)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside-login', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.inside_login:
        run_worker()
    else:
        from installer_common import build_output
        run_probe(build_output(args.output))
