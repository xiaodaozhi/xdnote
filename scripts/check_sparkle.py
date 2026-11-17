"""Native shared-session policy and loopback admission, without a product updater."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import platform
import plistlib
import re
import select
import shutil
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading

import sparkle


def execute(arguments, log, **options):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, timeout=60, **options)
    with log.open('ab') as output:
        output.write(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError('Native Sparkle probe command failed; see admission log')
    return result.stdout


@contextmanager
def listener(kind, tls=None, forward_port=None):
    events = []

    class Handler(socketserver.BaseRequestHandler):
        def tunnel(self):
            with socket.create_connection(('127.0.0.1', forward_port), timeout=3) as upstream:
                while True:
                    ready, _, _ = select.select([self.request, upstream], [], [], 3)
                    if not ready:
                        return
                    for source in ready:
                        block = source.recv(16384)
                        if not block:
                            return
                        (upstream if source is self.request else self.request).sendall(block)

        def handle(self):
            self.request.settimeout(3)
            try:
                if kind == 'tls':
                    events.append('connection')
                    try:
                        with tls.wrap_socket(self.request, server_side=True) as stream:
                            stream.recv(1024)
                    except ssl.SSLError as error:
                        events.append('tls-refused:' + str(error.reason))
                elif kind == 'http':
                    request = b''
                    while b'\r\n\r\n' not in request and len(request) < 8192:
                        block = self.request.recv(1024)
                        if not block:
                            break
                        request += block
                    events.append(request.split(b'\r\n')[0].decode('ascii', errors='replace'))
                    if forward_port is None:
                        self.request.sendall(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                    else:
                        if not request.startswith(f'CONNECT xdnote-sparkle-fixture.invalid:{forward_port} '.encode()):
                            raise OSError('Unexpected HTTP tunnel target')
                        self.request.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                        self.tunnel()
                else:
                    def read(length):
                        value = b''
                        while len(value) < length:
                            part = self.request.recv(length - len(value))
                            if not part:
                                raise OSError('Short SOCKS fixture request')
                            value += part
                        return value
                    greeting = read(2)
                    if greeting[0] != 5 or 0 not in read(greeting[1]):
                        raise OSError('Unexpected SOCKS greeting')
                    self.request.sendall(b'\x05\x00')
                    request = read(4)
                    if request[:3] != b'\x05\x01\x00':
                        raise OSError('Expected SOCKS CONNECT')
                    if request[3] == 1:
                        host = '.'.join(map(str, read(4)))
                    elif request[3] == 3:
                        host = read(read(1)[0]).decode('ascii')
                    else:
                        raise OSError('Unexpected SOCKS target address')
                    port = int.from_bytes(read(2), 'big')
                    events.append(f'CONNECT {host}:{port}')
                    if forward_port is None:
                        self.request.sendall(b'\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00')
                    else:
                        if host != 'xdnote-sparkle-fixture.invalid' or port != forward_port:
                            raise OSError('Unexpected SOCKS tunnel target')
                        self.request.sendall(b'\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00')
                        self.tunnel()
            except OSError:
                events.append('closed')

    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True

    with Server(('127.0.0.1', 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True)
        thread.start()
        try:
            yield server.server_address[1], events
        finally:
            server.shutdown()
            thread.join()


def native_metadata(sdk, log):
    library = sdk / 'Sparkle.framework/Versions/B/Sparkle'
    architectures = execute(['/usr/bin/lipo', '-archs', library], log).decode().split()
    if architectures != [platform.machine()]:
        raise ValueError('Sparkle main library architecture differs')
    commands = execute(['/usr/bin/otool', '-l', library], log).decode()
    minimum = re.findall(r'\bminos (\d+\.\d+)', commands)
    if minimum != ['13.0']:
        raise ValueError('Sparkle main library deployment target differs')
    imports = execute(['/usr/bin/otool', '-L', library], log).decode().splitlines()[1:]
    if any(not value.strip().startswith(('/System/Library/', '/usr/lib/', '@rpath/Sparkle.framework/')) for value in imports):
        raise ValueError('Sparkle main library imports a non-system runtime')
    execute(['/usr/bin/codesign', '--verify', '--deep', '--strict', sdk / 'Sparkle.framework'], log)
    helpers = {}
    for path in sorted((sdk / 'Sparkle.framework/Versions/B').rglob('*')):
        if not path.is_file() or path.is_symlink() or not path.stat().st_mode & 0o111 or path == library:
            continue
        magic = path.read_bytes()[:4]
        if magic not in (b'\xca\xfe\xba\xbe', b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf'):
            continue
        archs = execute(['/usr/bin/lipo', '-archs', path], log).decode().split()
        if not {'arm64', 'x86_64'}.issubset(archs):
            raise ValueError('Upstream Sparkle helper does not include both targets')
        code = execute(['/usr/bin/otool', '-arch', 'all', '-l', path], log).decode()
        targets = re.findall(r'\bminos (\d+\.\d+)', code)
        if len(targets) != 2 or any(tuple(map(int, value.split('.'))) > (13, 0) for value in targets):
            raise ValueError('Upstream Sparkle helper target exceeds macOS 13')
        signature = subprocess.run(['/usr/bin/codesign', '-dvv', '--entitlements', ':-', '--xml', str(path)],
                                   capture_output=True, check=True, timeout=30)
        entitlements = plistlib.loads(signature.stdout) if signature.stdout.strip() else {}
        flags = re.search(rb'flags=0x([0-9a-fA-F]+)', signature.stderr)
        if flags is None or not int(flags[1], 16) & 0x10000 or entitlements.get('com.apple.security.get-task-allow'):
            raise ValueError('Upstream Sparkle helper signing policy differs')
        helpers[path.relative_to(sdk).as_posix()] = {'sha256': sparkle.digest(path), 'architectures': archs,
            'minimumMacos': targets, 'signatureFlags': int(flags[1], 16), 'hardenedRuntime': True, 'entitlements': entitlements,
            'origin': 'pinned official SDK; unchanged'}
    if len(helpers) != 4:
        raise ValueError('Unexpected Sparkle helper set')
    return {'architecture': architectures, 'minimumMacos': minimum[0], 'imports': imports, 'helpers': helpers}


def check(work, evidence):
    sdk = sparkle.owned(sparkle.BASE / platform.machine())
    receipt = sparkle.verify(sdk, built=True)
    work, evidence = sparkle.owned(work), sparkle.owned(evidence)
    work.mkdir(parents=True, exist_ok=True)
    evidence.mkdir(parents=True, exist_ok=True)
    log = evidence / 'native.log'
    log.write_bytes(b'')
    result = {'ok': False, 'scope': 'native framework/session configuration and loopback; no product update or installation',
              'sourceRevision': subprocess.check_output(['git', '-C', str(sparkle.ROOT), 'rev-parse', 'HEAD'], encoding='utf-8').strip(),
              'sourceDirty': bool(subprocess.check_output(['git', '-C', str(sparkle.ROOT), 'status', '--porcelain'], encoding='utf-8').strip()),
              'probeSourceSha256': sparkle.digest(sparkle.ROOT / 'experiments/sparkle-admission/probe.m'),
              'checkScriptSha256': sparkle.digest(__file__),
              'frameworkBuild': receipt['frameworkBuild'], 'routes': {}}
    try:
        result['native'] = native_metadata(sdk, log)
        with tempfile.TemporaryDirectory(prefix='native-', dir=work) as directory:
            root = Path(directory)
            app = root / 'xdnote-sparkle-probe.app'
            executable = app / 'Contents/MacOS/xdnote-sparkle-probe'
            executable.parent.mkdir(parents=True)
            frameworks = app / 'Contents/Frameworks'
            frameworks.mkdir()
            shutil.copytree(sdk / 'Sparkle.framework', frameworks / 'Sparkle.framework', symlinks=True)
            # Public RFC 8032 test-vector key, isolated to this disposable probe.
            # It is never read by the product or a production trust-root loader.
            info = {'CFBundleIdentifier': 'org.xdnote.admission.sparkle', 'CFBundleExecutable': executable.name,
                    'CFBundlePackageType': 'APPL', 'CFBundleName': 'xdnote Sparkle admission',
                    'CFBundleVersion': '1', 'CFBundleShortVersionString': '0.1.0',
                    'SUPublicEDKey': '11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=',
                    'SUEnableAutomaticChecks': False, 'SUAutomaticallyUpdate': False,
                    'SUEnableInstallerLauncherService': False, 'SUEnableDownloaderService': False,
                    'SUFeedURL': 'https://127.0.0.1:1/never-requested'}
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            execute(['/usr/bin/xcrun', 'clang', '-fobjc-arc', '-fblocks', '-Wall', '-Wextra', '-Werror',
                     '-mmacosx-version-min=13.0', sparkle.ROOT / 'experiments/sparkle-admission/probe.m',
                     '-F', sdk, '-framework', 'Sparkle', '-framework', 'Foundation', '-framework', 'CFNetwork',
                     '-framework', 'Network', '-framework', 'Security', '-Wl,-rpath,@executable_path/../Frameworks', '-o', executable], log)
            # No Qt/other-project SDK fallbacks are available to the probe.
            environment = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'TMPDIR': str(root),
                           'CFFIXED_USER_HOME': str(root)}
            policy = json.loads(execute([executable, 'policy'], log, env=environment))
            if policy.get('ok') is not True:
                raise ValueError('Native Sparkle policy probe failed')
            result['policy'] = policy
            execute(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                     '-subj', '/CN=127.0.0.1', '-keyout', root / 'fixture.key', '-out', root / 'fixture.crt'], log)
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(root / 'fixture.crt', root / 'fixture.key')
            with listener('tls', tls) as (port, origin):
                url = f'https://127.0.0.1:{port}/private-loopback-fixture'
                direct = json.loads(execute([executable, 'request', 'direct', url], log, env=environment))
                if direct.get('error') not in (-1200, -1202) or not direct.get('sawUntrustedServerTrust') or not origin:
                    raise ValueError('Loopback untrusted TLS was not rejected')
                result['routes']['direct'] = {'response': direct, 'origin': list(origin)}
                # macOS may bypass proxies for loopback IP literals. Use the
                # reserved .invalid fixture name for CONNECT/SOCKS instead;
                # the rejecting local proxy never resolves or contacts it.
                proxy_url = f'https://xdnote-sparkle-fixture.invalid:{port}/fixture'
                commands = ['request-legacy']
                if tuple(map(int, platform.mac_ver()[0].split('.')[:2])) >= (14, 0):
                    commands.insert(0, 'request')
                for command in commands:
                    for kind, scheme in (('http', 'http'), ('socks', 'socks5')):
                        with listener(kind) as (proxy_port, requests):
                            before = len(origin)
                            response = json.loads(execute([executable, command, 'custom', proxy_url,
                                                  f'{scheme}://127.0.0.1:{proxy_port}'], log, env=environment))
                            expected = f'CONNECT xdnote-sparkle-fixture.invalid:{port}'
                            key = f'{command}-{scheme}'
                            result['routes'][key] = {'response': response, 'proxy': list(requests), 'originConnections': len(origin) - before}
                            if response.get('error') in (0, -1003) or not any(value.startswith(expected) for value in requests) or len(origin) != before:
                                raise ValueError('Native proxy route/failure isolation differs: ' + key)
                        with listener(kind, forward_port=port) as (proxy_port, requests):
                            before = len(origin)
                            response = json.loads(execute([executable, command, 'custom', proxy_url,
                                                  f'{scheme}://127.0.0.1:{proxy_port}'], log, env=environment))
                            key = f'{command}-{scheme}-tls'
                            result['routes'][key] = {'response': response, 'proxy': list(requests), 'originConnections': len(origin) - before}
                            if response.get('error') not in (-1200, -1202) or not response.get('sawUntrustedServerTrust') or not any(value.startswith(expected) for value in requests) or len(origin) <= before:
                                raise ValueError('Native proxy tunnel did not retain TLS trust: ' + key)
            result['ok'] = True
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        (evidence / 'report.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': result['ok'], 'policyChecks': len(result['policy']['checks']), 'routes': list(result['routes'])}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path)
    args = parser.parse_args()
    check(args.work_dir, args.evidence)
