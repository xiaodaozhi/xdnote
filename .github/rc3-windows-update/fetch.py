"""Verify an exact-byte deploy key and fetch the sealed candidate over pinned SSH."""

import argparse
import base64
import csv
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


SECRET = 'XDNOTE_WINDOWS_DIAGNOSTIC_CANDIDATE_KEY'
GITHUB_KNOWN_HOST = (
    '[ssh.github.com]:443 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n'
)


def restrict_directory(directory):
    if os.name == 'nt':
        account = subprocess.check_output(['whoami.exe', '/user', '/fo', 'csv', '/nh'], text=True, timeout=10)
        _, sid = next(csv.reader(account.splitlines()))
        if not re.fullmatch(r'S-1-\d+(?:-\d+)+', sid):
            raise ValueError('Cannot determine the Windows runner SID')
        subprocess.run(['icacls.exe', directory, '/inheritance:r', '/grant:r', f'*{sid}:(OI)(CI)F'],
                       check=True, stdout=subprocess.DEVNULL, timeout=15)
        return sid
    directory.chmod(0o700)
    return None


def fingerprint(public):
    kind, wire = public
    if kind != 'ssh-ed25519':
        raise ValueError('Expected an Ed25519 public key')
    decoded = base64.b64decode(wire, validate=True)
    if len(decoded) != 51 or not decoded.startswith(b'\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20'):
        raise ValueError('Unexpected Ed25519 public key structure')
    return 'SHA256:' + base64.b64encode(hashlib.sha256(decoded).digest()).decode('ascii').rstrip('=')


def sanitized_error(stderr):
    text = stderr.decode('utf-8', errors='replace').lower()
    if not text.strip():
        return None
    known = {
        'bad permissions': 'unsafe_permissions',
        'permissions are too open': 'unsafe_permissions',
        'unprotected private key': 'unsafe_permissions',
        'bad owner': 'unsafe_permissions',
        'permission denied': 'permission_denied',
        'access is denied': 'permission_denied',
        'invalid format': 'invalid_format',
        'error in libcrypto': 'crypto_error',
        'incorrect passphrase': 'passphrase_error',
        'no such file': 'file_not_found',
        'cannot find': 'file_not_found',
        'invalid argument': 'invalid_argument',
    }
    return next((label for fragment, label in known.items() if fragment in text), 'unclassified_stderr')


def ssh_clients():
    candidates = [shutil.which('ssh')]
    if os.name == 'nt':
        git = Path(shutil.which('git.exe')).resolve()
        candidates += [Path(os.environ['SystemRoot']) / 'System32/OpenSSH/ssh.exe',
                       git.parent.parent / 'usr/bin/ssh.exe']
    return list(dict.fromkeys(Path(path).resolve() for path in candidates if path and Path(path).is_file()))


def derive_public_key(ssh, key, expected, report):
    keygen = ssh.with_name('ssh-keygen' + ssh.suffix)
    record = {'sshKeygen': str(keygen), 'exitCode': None, 'sanitizedError': None,
              'expectedFingerprint': fingerprint(expected), 'derivedFingerprint': None}
    try:
        result = subprocess.run([keygen, '-y', '-f', key.as_posix()], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        record.update(outcome='read_failed', sanitizedError='ssh_keygen_unavailable_or_timed_out')
    else:
        record.update(exitCode=result.returncode, sanitizedError=sanitized_error(result.stderr))
        if result.returncode:
            record['outcome'] = 'read_failed'
        else:
            try:
                derived = result.stdout.decode('ascii').split()[:2]
                record['derivedFingerprint'] = fingerprint(derived)
            except ValueError:
                record['outcome'] = 'invalid_public_key_output'
            else:
                record['outcome'] = 'match' if derived == expected else 'public_key_mismatch'
    report['keygenAttempts'].append(record)
    print(json.dumps(record), flush=True)
    return record


def select_ssh(key, sid, expected, report):
    for ssh in ssh_clients():
        record = derive_public_key(ssh, key, expected, report)
        if sid and record['outcome'] == 'read_failed' and record['sanitizedError'] in {'unsafe_permissions', 'permission_denied'}:
            subprocess.run(['icacls.exe', key, '/inheritance:r', '/grant:r', f'*{sid}:F'],
                           check=True, stdout=subprocess.DEVNULL, timeout=15)
            record = derive_public_key(ssh, key, expected, report)
        if record['outcome'] == 'match':
            return ssh
        if record['outcome'] != 'read_failed':
            raise ValueError(record['outcome'])
    raise ValueError('No installed ssh-keygen could read the restored private key')


def fetch(destination, check_only, report):
    credential = json.loads(os.environ.pop(SECRET))
    expected = credential['binding']
    if os.environ['XDNOTE_TRANSFER_COMMIT'] != expected['commit']:
        raise ValueError('Workflow and credential transfer commits differ')
    repository = expected['repository']
    commit = expected['commit']
    if not re.fullmatch(r'[A-Za-z0-9-]+/xdnote-candidate-transfer-[0-9a-f]{12}', repository):
        raise ValueError('Unexpected private transport repository')
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected a frozen private transfer commit')
    key_bytes = base64.b64decode(credential['bytesBase64'], validate=True)
    report['secretBytesVerified'] = hmac.compare_digest(hashlib.sha256(key_bytes).hexdigest(), credential['sha256'])
    if not report['secretBytesVerified']:
        raise ValueError('Restored private key bytes differ from the accompanying digest')
    expected_public = expected['deployPublicKey'].split()
    report['expectedFingerprint'] = fingerprint(expected_public)
    remote = f'git@github.com:{repository}.git'

    with tempfile.TemporaryDirectory(prefix='xdnote-candidate-ssh-', dir=os.environ.get('RUNNER_TEMP')) as temporary:
        directory = Path(temporary)
        sid = restrict_directory(directory)
        key = directory / 'deploy-key'
        key.write_bytes(key_bytes)
        if os.name != 'nt':
            key.chmod(0o600)
        report['writtenBytesVerified'] = hmac.compare_digest(key.read_bytes(), key_bytes)
        if not report['writtenBytesVerified']:
            raise ValueError('The key file differs from the decoded bytes')
        ssh = select_ssh(key, sid, expected_public, report)

        known = directory / 'known_hosts'
        known.write_bytes(GITHUB_KNOWN_HOST.encode('ascii'))
        config = directory / 'config'
        config.write_bytes((
            'Host github.com\n'
            '    HostName ssh.github.com\n'
            '    Port 443\n'
            '    User git\n'
            '    BatchMode yes\n'
            '    IdentitiesOnly yes\n'
            '    StrictHostKeyChecking yes\n'
            '    HostKeyAlgorithms ssh-ed25519\n'
            '    GlobalKnownHostsFile none\n'
            '    ConnectTimeout 15\n'
            f'    IdentityFile "{key.as_posix()}"\n'
            f'    UserKnownHostsFile "{known.as_posix()}"\n'
        ).encode('utf-8'))
        environment = dict(os.environ,
                           GIT_SSH_COMMAND=f'{shlex.quote(ssh.as_posix())} -F {shlex.quote(config.as_posix())}',
                           GIT_SSH_VARIANT='ssh', GIT_TERMINAL_PROMPT='0')
        remote_head = subprocess.check_output(
            ['git', 'ls-remote', '--exit-code', remote, 'refs/heads/main'],
            env=environment, text=True, timeout=35).strip()
        if remote_head.split() != [commit, 'refs/heads/main']:
            raise ValueError('Authenticated candidate branch differs from its pinned commit')
        report['remoteCommitVerified'] = True
        print('Read-only SSH access and the exact remote candidate commit verified.', flush=True)
        if check_only:
            return
        destination.mkdir()
        subprocess.run(['git', 'init', '--quiet', '--initial-branch=main', destination], check=True, timeout=15)
        git = ['git', '-C', str(destination)]
        subprocess.run([*git, 'fetch', '--no-tags', '--depth=1', '--no-recurse-submodules', remote, commit],
                       env=environment, check=True, timeout=240)
        fetched = subprocess.check_output([*git, 'rev-parse', 'FETCH_HEAD'], text=True, timeout=15).strip()
        if fetched != commit:
            raise ValueError('Fetched candidate differs from its pinned commit')
        subprocess.run([*git, 'update-ref', 'refs/heads/main', commit], check=True, timeout=15)
        report['fetchedCommitVerified'] = True


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    report = {'secretBytesVerified': False, 'writtenBytesVerified': False, 'keygenAttempts': [],
              'remoteCommitVerified': False, 'fetchedCommitVerified': False}
    try:
        fetch(args.destination, args.check_only, report)
    finally:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
