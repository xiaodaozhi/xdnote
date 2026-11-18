"""Private proxies only forward the declared owned loopback origin."""
import socket
import socketserver
import struct
import threading
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native_fixture_proxy import Proxy, HOST, read


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.server.requests += 1
        if read(self.request, 4) == b'PING': self.request.sendall(b'PONG')


class NativeProxyTests(unittest.TestCase):
    def test_connect_and_socks_only_reach_owned_target(self):
        origin = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo); origin.requests = 0
        threading.Thread(target=origin.serve_forever, daemon=True).start()
        try:
            for mode in ['http', 'socks5']:
                proxy = Proxy(mode, origin.server_address[1]); threading.Thread(target=proxy.serve_forever, daemon=True).start()
                try:
                    for host, accepted in [(HOST, True), ('outside.invalid', False)]:
                        with socket.create_connection(proxy.server_address, timeout=5) as client:
                            if mode == 'http':
                                client.sendall(f'CONNECT {host}:{origin.server_address[1]} HTTP/1.1\r\n\r\n'.encode()); header = b''
                                while not header.endswith(b'\r\n\r\n'): header += read(client, 1)
                                self.assertIn(b'200' if accepted else b'502', header)
                            else:
                                client.sendall(b'\x05\x01\x00'); self.assertEqual(read(client, 2), b'\x05\x00')
                                client.sendall(b'\x05\x01\x00\x03' + bytes([len(host)]) + host.encode() + struct.pack('!H', origin.server_address[1]))
                                self.assertEqual(read(client, 10)[1], 0 if accepted else 4)
                            if accepted: client.sendall(b'PING'); self.assertEqual(read(client, 4), b'PONG')
                    self.assertEqual([r['accepted'] for r in proxy.records], [True, False])
                finally: proxy.shutdown(); proxy.server_close()
            self.assertEqual(origin.requests, 2)
        finally: origin.shutdown(); origin.server_close()
