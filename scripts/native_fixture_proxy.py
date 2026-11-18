"""Loopback-only proxy for private native-update fixtures, never a product API."""
import select
import socket
import socketserver
import struct
import time
import http.server
from pathlib import Path
from urllib.parse import urlsplit

HOST = 'updates.fixture.invalid'


def read(stream, count):
    result = b''
    while len(result) < count:
        part = stream.recv(count - len(result))
        if not part: raise ConnectionError('Truncated fixture handshake')
        result += part
    return result


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self, mode, origin_port, reject=False):
        if mode not in ('http', 'socks5') or not 1 <= origin_port <= 65535: raise ValueError('Bounded fixture proxy required')
        self.mode = mode; self.origin_port = origin_port; self.reject = reject; self.records = []; self.errors = []
        super().__init__(('127.0.0.1', 0), Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        client = self.request; client.settimeout(20)
        try:
            initial = b''
            if self.server.mode == 'http':
                header = b''
                while not header.endswith(b'\r\n\r\n'):
                    header += read(client, 1)
                    if len(header) > 8192: raise ValueError('Oversized fixture CONNECT')
                parts = header.split(b'\r\n', 1)[0].decode('ascii').split()
                if len(parts) != 3: raise ValueError('Invalid fixture proxy request')
                if parts[0] == 'CONNECT': host, port = parts[1].rsplit(':', 1); port = int(port)
                elif parts[0] == 'GET':
                    url = urlsplit(parts[1]); host, port = url.hostname, url.port
                    if url.scheme != 'http' or url.username or url.password or url.query or url.fragment or url.path != '/archive.app.zip': raise ValueError('Only owned fixture HTTP package route allowed')
                    initial = b'GET /archive.app.zip ' + parts[2].encode() + b'\r\n' + header.split(b'\r\n', 1)[1]
                else: raise ValueError('Fixture requires GET or CONNECT')
            else:
                version, count = read(client, 2)
                if version != 5 or 0 not in read(client, count): raise ValueError('No proxy credentials accepted')
                client.sendall(b'\x05\x00'); version, command, reserved, kind = read(client, 4)
                if (version, command, reserved, kind) != (5, 1, 0, 3): raise ValueError('Domain SOCKS5 CONNECT required')
                host = read(client, read(client, 1)[0]).decode('ascii'); port = struct.unpack('!H', read(client, 2))[0]
            record = {'mode': self.server.mode, 'protocol': 'HTTP-forward' if initial else 'CONNECT', 'host': host, 'port': port, 'accepted': False}
            self.server.records.append(record)
            if host != HOST or port != self.server.origin_port or self.server.reject:
                client.sendall(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n' if self.server.mode == 'http' else b'\x05\x04\x00\x01\x00\x00\x00\x00\x00\x00')
                return
            with socket.create_connection(('127.0.0.1', port), timeout=15) as remote:
                record.update(accepted=True, originClientPort=remote.getsockname()[1])
                if initial: remote.sendall(initial)
                else: client.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n' if self.server.mode == 'http' else b'\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00')
                started = time.monotonic()
                while time.monotonic() - started < 120:
                    ready, _, _ = select.select([client, remote], [], [], 1)
                    for source in ready:
                        data = source.recv(65536)
                        if not data: return
                        (remote if source is client else client).sendall(data)
        except (OSError, ValueError) as error:
            self.server.errors.append(type(error).__name__)


class Origin(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self, package):
        self.package = Path(package); self.requests = []; self.disconnect_archive = False
        super().__init__(('127.0.0.1', 0), OriginHandler)


class OriginHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path != '/archive.app.zip': self.send_error(404); return
        self.server.requests.append({'path': self.path, 'clientPort': self.client_address[1]})
        path = self.server.package
        if path.is_symlink() or not path.is_file(): self.send_error(404); return
        self.send_response(200); self.send_header('Content-Length', str(path.stat().st_size)); self.end_headers()
        with path.open('rb') as source:
            while data := source.read(65536):
                try: self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError): break
                if self.server.disconnect_archive: self.close_connection = True; break
