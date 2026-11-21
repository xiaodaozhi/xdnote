"""Deliver only guest-requested focus input to the disposable VM's own screen."""
import json
from pathlib import Path
import socket
import sys
import time


class Console:
    def __init__(self, root):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(root / 'qmp'))
        self.stream = self.socket.makefile('rwb')
        if 'QMP' not in json.loads(self.stream.readline()):
            raise RuntimeError('Missing QMP greeting')
        self.call('qmp_capabilities')

    def call(self, command, **arguments):
        self.stream.write(json.dumps({'execute': command, 'arguments': arguments}).encode() + b'\n')
        self.stream.flush()
        while line := self.stream.readline():
            result = json.loads(line)
            if 'error' in result:
                raise RuntimeError(result['error'])
            if 'return' in result:
                return result['return']
        raise RuntimeError('QMP connection closed')

    def screenshot_size(self, path):
        self.call('screendump', filename=str(path))
        with path.open('rb') as image:
            if image.readline().strip() != b'P6':
                raise ValueError('Expected a PPM guest screenshot')
            width, height = map(int, image.readline().split())
            if image.readline().strip() != b'255' or len(image.read()) != width * height * 3:
                raise ValueError('Incomplete guest screenshot')
        return width, height

    def click(self, x, y, width, height):
        self.call('input-send-event', events=[
            {'type': 'abs', 'data': {'axis': axis, 'value': round(coord * 32767 / (size - 1))}}
            for axis, coord, size in (('x', x, width), ('y', y, height))
        ])
        time.sleep(.15)
        for down in (True, False):
            self.call('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])
            time.sleep(.12)


def main(root):
    root = Path(root)
    console = Console(root)
    serial = root / 'serial.log'
    evidence = root / 'evidence'
    cursor = 0
    history = []
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        text = serial.read_text(errors='replace')
        lines = text.rsplit('\n', 1)[0].splitlines() if '\n' in text else []
        for line in lines[cursor:]:
            if line.startswith('XDNOTE_VM_RESULT '):
                (evidence / 'focus-input.json').write_text(json.dumps(history, indent=2) + '\n')
                return
            if not line.startswith('XDNOTE_VM_INPUT '):
                continue
            request = line.removeprefix('XDNOTE_VM_INPUT ').split()
            frame = evidence / f'focus-{len(history) + 1}.ppm'
            if request == ['escape']:
                console.call('send-key', keys=[{'type': 'qcode', 'data': 'esc'}], **{'hold-time': 100})
                history.append({'action': 'dismiss-system-menu'})
            elif request == ['pinyin-shift']:
                console.call('send-key', keys=[{'type': 'qcode', 'data': 'shift'}], **{'hold-time': 120})
                history.append({'action': 'switch-pinyin-mode'})
            elif request == ['pinyin-nihao']:
                for key in 'nihao':
                    console.call('send-key', keys=[{'type': 'qcode', 'data': key}], **{'hold-time': 100})
                    time.sleep(.15)
                history.append({'action': 'type-pinyin'})
            elif request == ['pinyin-commit']:
                console.call('send-key', keys=[{'type': 'qcode', 'data': 'spc'}], **{'hold-time': 100})
                history.append({'action': 'confirm-pinyin'})
            elif request == ['snapshot']:
                width, height = console.screenshot_size(frame)
                history.append({'action': 'capture-failure', 'width': width, 'height': height})
            elif len(request) == 5 and request[0] == 'click':
                x, y, width, height = map(int, request[1:])
                if not (900 <= width <= 1920 and 650 <= height <= 1200 and 0 <= x < width and 0 <= y < height):
                    raise ValueError('Invalid bounded guest focus request')
                if console.screenshot_size(frame) != (width, height):
                    raise ValueError('The requested screen size differs from the guest console')
                console.click(x, y, width, height)
                history.append({'action': 'activate-owned-window', 'x': x, 'y': y})
            else:
                raise ValueError('Unexpected guest focus request')
        cursor = len(lines)
        time.sleep(.15)
    raise TimeoutError('The isolated guest did not finish its input-method check')


if __name__ == '__main__':
    main(sys.argv[1])
