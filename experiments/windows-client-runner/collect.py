import argparse
import base64
import json
from pathlib import Path
import socket
import time

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('operation', choices=('ready', 'attach', 'result', 'screenshot', 'quit'))
args = parser.parse_args()
root = args.directory
if args.operation in ('attach', 'screenshot', 'quit'):
    command = {
        'attach': f'change transfer {root}/transfer.iso',
        'screenshot': f'screendump {root}/evidence/screen.ppm',
        'quit': 'quit',
    }[args.operation]
    with socket.socket(socket.AF_UNIX) as monitor:
        monitor.settimeout(2)
        monitor.connect(str(root / 'monitor'))
        monitor.recv(4096)
        monitor.sendall(command.encode('ascii') + b'\n')
        if args.operation != 'quit':
            time.sleep(0.6)
            print(monitor.recv(4096).decode('ascii', errors='replace')[-650:])
else:
    marker = {'ready':'XDNOTE_VM_READY ', 'result':'XDNOTE_VM_RESULT '}[args.operation]
    deadline = time.monotonic() + (960 if args.operation == 'ready' else 540)
    while time.monotonic() < deadline:
        for line in (root / 'serial.log').read_text(errors='replace').splitlines():
            if line.startswith(marker):
                try:
                    record = json.loads(base64.b64decode(line.removeprefix(marker), validate=True))
                except ValueError:
                    continue
                (root / 'evidence' / (args.operation + '.json')).write_text(json.dumps(record, ensure_ascii=False, indent=2))
                if args.operation == 'result' and not record['ok']:
                    raise SystemExit(record.get('error', 'Windows client acceptance failed'))
                print(json.dumps(record, ensure_ascii=True)[:1800])
                raise SystemExit(0)
        time.sleep(2)
    raise SystemExit(f'No {args.operation} result from the isolated Windows guest')
