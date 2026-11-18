"""Exercise owned unpaced HTTP/TLS and bounded SOCKS5 buffers with pinned Qt.

No production root, system proxy, certificate store or installed application is
read or changed. Test keys exist only in the new owned output and are removed.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import socket
import socketserver
import ssl
import subprocess
import threading

import desktop
from installer_common import ROOT, build_output, digest
from native_fixture_proxy import Proxy, HOST, read
from qt_network_backports import verify

BLOCK = b'a' * 65536


def expected_hash(size):
    hasher = hashlib.sha256()
    while size:
        part = BLOCK[:min(size, len(BLOCK))]; hasher.update(part); size -= len(part)
    return hasher.hexdigest()


class Origin(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self, size, mode, close, raw=False):
        self.size = size; self.mode = mode; self.close = close; self.raw = raw; self.requests = []
        super().__init__(('127.0.0.1', 0), Payload if raw else HttpPayload)
    def shutdown_request(self, request):
        if isinstance(request, ssl.SSLSocket) and self.close and self.mode == 'valid':
            try:
                request.settimeout(3)
                request.unwrap().close()
            except (OSError, ssl.SSLError): request.close()
        else: super().shutdown_request(request)


def write_payload(handler):
    size = handler.server.size
    count = size // 2 if handler.server.mode == 'truncated' else size + (handler.server.mode == 'oversized')
    first = True
    try:
        while count:
            data = BLOCK[:min(count, len(BLOCK))]
            if first and handler.server.mode == 'corrupt': data = b'b' + data[1:]
            handler.wfile.write(data); count -= len(data); first = False
    except (OSError, ssl.SSLError): pass


class HttpPayload(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path != '/archive.app.zip': self.send_error(404); return
        self.server.requests.append({'clientPort': self.client_address[1], 'path': self.path})
        self.send_response(200)
        self.send_header('Content-Length', str(self.server.size + (self.server.mode == 'oversized')))
        if self.server.close or self.server.mode == 'truncated': self.send_header('Connection', 'close')
        self.end_headers(); write_payload(self)
        self.close_connection = self.server.close or self.server.mode == 'truncated'


class Payload(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        if read(self.request, 4) != b'PING': return
        self.server.requests.append({'clientPort': self.client_address[1], 'path': 'owned raw payload'})
        write_payload(self)
        if not self.server.close:
            try: self.request.recv(1)
            except OSError: pass


def certificates(work, env):
    try: tool = desktop.prepared_tool('openssl', env)
    except RuntimeError:
        candidates = [Path('C:/Program Files/OpenSSL/bin/openssl.exe'), Path('C:/Program Files/Git/usr/bin/openssl.exe')]
        found = next((p for p in candidates if platform.system() == 'Windows' and p.is_file()), None)
        if found is None: raise
        tool = str(desktop.checked_tool_path(found, 'System OpenSSL diagnostic tool'))
    def run(*args):
        subprocess.run([tool, *map(str, args)], env=env, check=True, capture_output=True, timeout=30)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', work/'ca-key.pem', '-out', work/'ca.pem',
        '-days', '1', '-subj', '/CN=xdnote owned transport fixture', '-addext', 'basicConstraints=critical,CA:TRUE',
        '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
    run('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-keyout', work/'server-key.pem', '-out', work/'server.csr',
        '-subj', '/CN=updates.fixture.invalid')
    for name, host in [('server', HOST), ('wrong-host', 'wrong.fixture.invalid')]:
        extensions = work/(name+'.ext')
        extensions.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:'+host+'\n')
        run('x509', '-req', '-in', work/'server.csr', '-CA', work/'ca.pem', '-CAkey', work/'ca-key.pem', '-CAcreateserial',
            '-out', work/(name+'.pem'), '-days', '1', '-extfile', extensions)
    run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', work/'other-ca-key.pem', '-out', work/'other-ca.pem',
        '-days', '1', '-subj', '/CN=xdnote other owned fixture', '-addext', 'basicConstraints=critical,CA:TRUE')
    for p in work.glob('*key.pem'): p.chmod(0o600)
    (work/'ca-key.pem').unlink(); (work/'other-ca-key.pem').unlink()


def check(qt, work, vcvars=None):
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True); work = build_output(work)
    if work.exists(): raise ValueError('Preserve existing flow evidence')
    work.mkdir(parents=True)
    receipt = verify(qt); env = desktop.environment(vcvars)
    for key in list(env):
        if key.upper().startswith(('QT_', 'QML_', 'LD_', 'DYLD_')): env.pop(key)
    env = desktop.runtime_environment(env, qt)
    report = {'ok': False, 'sdkVariant': receipt, 'cases': {}, 'releaseReady': False,
              'sourceRevision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'probeSources': {p.name: digest(p) for p in (ROOT/'experiments/update-network-flow').glob('*') if p.is_file()},
              'scope': 'owned unpaced transport, TLS rejection and private socket bounds; no native install/production/minimum-OS acceptance'}
    cmake = desktop.prepared_tool('cmake', env); ninja = desktop.prepared_tool('ninja', env); build = work/'build'
    def command(args, log, timeout=120):
        run = subprocess.run(list(map(str, args)), env=env, capture_output=True, timeout=timeout)
        (work/log).write_bytes(run.stdout + run.stderr)
        if len(run.stdout) + len(run.stderr) > 65536: raise ValueError('Unbounded probe output')
        return run
    args = [cmake, '-S', ROOT/'experiments/update-network-flow', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_PREFIX_PATH='+str(qt), '-DCMAKE_MAKE_PROGRAM='+ninja]
    if platform.system() == 'Darwin': args += ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0']
    if command(args, 'configure.log').returncode or command([cmake, '--build', build, '--parallel', '2'], 'build.log').returncode:
        raise RuntimeError('Flow probe build failed; see owned logs')
    suffix = '.exe' if platform.system() == 'Windows' else ''
    def scenario(key, transport, size, mode='valid', close=False, pause=0, tls=False, raw=False, blocking=False):
        origin = Origin(size, mode, close, raw); proxy = Proxy(transport, origin.server_address[1])
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(work/('wrong-host.pem' if mode == 'hostname' else 'server.pem'), work/'server-key.pem')
            origin.socket = context.wrap_socket(origin.socket, server_side=True)
        for server in [origin, proxy]: threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            exe = build/(('update_network_socket_bounds' if raw else 'update_network_flow')+suffix)
            args = [exe, '--proxy', f'{transport}://127.0.0.1:{proxy.server_address[1]}', '--bytes', size,
                    '--sha256', expected_hash(size), '--pause-ms', pause]
            if raw:
                args += ['--port', origin.server_address[1]]
                if close: args += ['--remote-closes']
                if blocking: args += ['--blocking']
            else:
                args += ['--url', f'{"https" if tls else "http"}://{HOST}:{origin.server_address[1]}/archive.app.zip',
                         '--ca', work/('other-ca.pem' if mode == 'untrusted' else 'ca.pem')]
            result = command(args, key+'.log', 35)
            data = json.loads(result.stdout)
            positive = mode == 'valid'
            if data.get('ok') is not positive or result.returncode != (0 if positive else 2): raise ValueError('Flow expectation failed: '+key)
            if not raw:
                if data['readBufferBytes'] != 65536: raise ValueError('Download buffer bound changed')
                path = Path(data['networkLibrary']).resolve()
                if not path.is_relative_to(qt): raise ValueError('Probe loaded a different SDK Network')
                if mode in ('hostname', 'untrusted') and not data['tlsError']: raise ValueError('TLS rejection not observed')
            ports = {r['originClientPort'] for r in proxy.records if r['accepted']}
            if not ports or (positive and (not origin.requests or any(r['clientPort'] not in ports for r in origin.requests))):
                raise ValueError('Flow escaped the observed owned proxy')
            report['cases'][key] = {'result': data, 'proxy': proxy.records, 'origin': origin.requests, 'proxyErrors': proxy.errors}
            (work/'report.json').write_text(json.dumps(report, indent=2)+'\n'); print(key+' passed', flush=True)
        finally:
            for server in [proxy, origin]: server.shutdown(); server.server_close()
    try:
        certificates(work, env)
        for transport in ['http', 'socks5']:
            for tls in [False, True]:
                prefix = transport+('-tls' if tls else '-plain')
                scenario(prefix+'-256MiB', transport, 256*1024*1024, tls=tls)
                scenario(prefix+'-close-paused', transport, 32*1024*1024, close=True, pause=300, tls=tls)
                for mode in ['truncated', 'corrupt', 'oversized']:
                    scenario(prefix+'-'+mode, transport, 2*1024*1024, mode=mode, tls=tls)
            for mode in ['hostname', 'untrusted']:
                scenario(transport+'-tls-'+mode, transport, 2*1024*1024, mode=mode, tls=True)
        for blocking in [False, True]:
            for close in [False, True]:
                scenario(f'socket-{blocking}-{close}', 'socks5', 4*1024*1024, close=close, pause=300, raw=True, blocking=blocking)
        scenario('socket-exact-buffer-eof', 'socks5', 4096, close=True, pause=300, raw=True)
        report['ok'] = True
    finally:
        for path in work.glob('*key.pem'): path.unlink()
        report['privateKeysRemoved'] = not list(work.glob('*key.pem'))
        (work/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qt-prefix', type=Path, required=True); p.add_argument('--work-dir', type=Path, required=True); p.add_argument('--vcvars', type=Path)
    a = p.parse_args(); check(a.qt_prefix, a.work_dir, a.vcvars)
