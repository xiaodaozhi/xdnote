"""Record actual Qt HTTPS and HTTP CONNECT/SOCKS5 routes to fixed public hosts."""
import argparse
import json
from pathlib import Path
import select
import socket
import socketserver
import struct
import subprocess
import threading
import time
import desktop
from installer_common import build_output


def read(stream, length):
    result=b''
    while len(result)<length:
        value=stream.recv(length-len(result))
        if not value: raise ConnectionError('Short proxy handshake')
        result+=value
    return result


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    def __init__(self, scheme):
        self.scheme=scheme; self.destinations=[]; self.errors=[]
        super().__init__(('127.0.0.1',0),Handler)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        client=self.request; client.settimeout(20)
        try:
            if self.server.scheme=='http':
                header=b''
                while not header.endswith(b'\r\n\r\n'):
                    header+=read(client,1)
                    if len(header)>8192: raise ValueError('Oversized CONNECT')
                parts=header.split(b'\r\n',1)[0].decode('ascii').split()
                if len(parts)!=3 or parts[0]!='CONNECT': raise ValueError('CONNECT required')
                host,port=parts[1].rsplit(':',1); port=int(port)
            else:
                version,count=read(client,2)
                methods=read(client,count)
                if version!=5 or 0 not in methods: raise ValueError('Unauthenticated SOCKS5 required')
                client.sendall(b'\x05\x00'); version,command,reserved,kind=read(client,4)
                if (version,command,reserved,kind)!=(5,1,0,3): raise ValueError('Domain SOCKS5 CONNECT required')
                host=read(client,read(client,1)[0]).decode('ascii'); port=struct.unpack('!H',read(client,2))[0]
            if host not in ('github.com','api.github.com') or port!=443: raise ValueError('Destination outside fixed public route')
            with socket.create_connection((host,port),timeout=15) as remote:
                self.server.destinations.append({'host':host,'port':port})
                client.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n' if self.server.scheme=='http' else b'\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00')
                started=time.monotonic()
                while time.monotonic()-started<90:
                    ready,_,_=select.select([client,remote],[],[],1)
                    for source in ready:
                        data=source.recv(65536)
                        if not data:return
                        (remote if source is client else client).sendall(data)
        except (OSError,ValueError) as error:self.server.errors.append(type(error).__name__)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','qt-prefix','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args(); build=desktop.build_path(a.build_dir);qt=desktop.checked_tool_path(a.qt_prefix,'Qt SDK',sdk=True)
    identity=desktop.verify_build_identity(build,'default',qt); output=build_output(a.output)
    if output.exists():p.error('Preserve existing route evidence')
    output.mkdir(parents=True);env=desktop.runtime_environment(desktop.environment(None),qt)
    executable=build/('xdnote_update_probe.exe' if __import__('platform').system()=='Windows' else 'xdnote_update_probe')
    records=[]
    for mode in ('system','none','http','socks5'):
        server=Proxy(mode) if mode in ('http','socks5') else None
        if server:threading.Thread(target=server.serve_forever,daemon=True).start()
        try:
            for page in (True,False):
                address=f'{mode}://127.0.0.1:{server.server_address[1]}' if server else ''
                args=[str(executable),'custom' if server else mode,address]+(['--public-page'] if page else [])
                run=subprocess.run(args,env=env,capture_output=True,text=True,timeout=370)
                values=[json.loads(line) for line in run.stdout.splitlines() if line.startswith('{')]
                record={'mode':mode,'kind':'public-page' if page else 'update-check','exit':run.returncode,'result':values[-1] if values else {}}
                if server:record['destinations']=list(server.destinations);record['proxyErrors']=list(server.errors)
                records.append(record); print(json.dumps(record),flush=True)
        finally:
            if server:server.shutdown();server.server_close()
    result={'buildIdentity':identity,'routes':records,'normalPublicHttps':any(r['result'].get('ok') for r in records if r['kind']=='public-page'),
            'normalUpdateCheck':any(r['exit']==0 for r in records if r['kind']=='update-check'),
            'scope':'actual public TLS and observed local proxy tunnels; no release/package installation or minimum-OS acceptance'}
    (output/'report.json').write_text(json.dumps(result,indent=2)+'\n')
