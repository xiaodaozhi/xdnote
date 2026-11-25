"""Measure Orca speech on the single owned Quick editor in the private X11 session."""

import os
from pathlib import Path
import subprocess
import sys
import time

from atspi_client import Atspi, find_editor, require_text, wait_for_command


TITLE = 'xdnote Orca research fixture'
ORIGINAL = 'Blue orchard old document'
REPLACEMENT = 'Golden willow replacement sample'
WRITTEN = 'Silver birch accessible write'


def xdotool(*arguments):
    return subprocess.check_output(['xdotool', *arguments], text=True, timeout=5).strip()


def main(pid):
    Atspi.set_timeout(2000, 5000)
    editor = find_editor(pid, TITLE)
    windows = xdotool('search', '--onlyvisible', '--all', '--pid', str(pid), '--name', '^' + TITLE + '$').splitlines()
    if len(windows) != 1 or xdotool('getwindowpid', windows[0]) != str(pid):
        raise RuntimeError('Expected one X11 fixture window owned by the parent test')
    window = windows[0]
    xdotool('windowactivate', '--sync', window)
    xdotool('mousemove', '--window', window, '40', '15', 'click', '1')
    time.sleep(1)
    log = Path(os.environ['XDNOTE_QUICK_ORCA_LOG'])
    speech_probe = Path(os.environ['XDNOTE_ORCA_SPEECH_PROBE'])

    def send_keys(*keys):
        if xdotool('getactivewindow') != window:
            raise RuntimeError('The isolated fixture lost focus')
        xdotool('key', '--clearmodifiers', *keys)

    def hear(phase, expected):
        text = require_text(editor, expected)
        length = text.characterCount
        selected = text.setSelection(0, 0, length) if text.getNSelections() else text.addSelection(0, length)
        if not selected or tuple(text.getSelection(0)) != (0, length):
            raise RuntimeError('Could not select the fixture text for Orca')
        time.sleep(0.5)
        offset = log.stat().st_size
        send_keys('Insert+Shift+Up')
        subprocess.run(
            [sys.executable, str(speech_probe), 'speech', str(log), phase, str(offset), expected],
            stdout=sys.stderr, check=True, timeout=17,
        )
        require_text(editor, expected)

    hear('old-document', ORIGINAL)
    print('OLD_SPOKEN', flush=True)

    wait_for_command('READ_ONLY')
    hear('read-only-replacement', REPLACEMENT)
    send_keys('End', 'z')
    require_text(editor, REPLACEMENT)
    print('REPLACEMENT_SPOKEN', flush=True)

    wait_for_command('WRITE')
    editor.clear_cache()
    if not editor.queryEditableText().setTextContents(WRITTEN):
        raise RuntimeError('The controlled AT-SPI client could not set the editable text')
    hear('after-external-write', WRITTEN)
    print('WRITE_SPOKEN', flush=True)

    wait_for_command('UNDO')
    hear('after-two-undos', 'Golden willow')
    print('UNDO_SPOKEN', flush=True)


if __name__ == '__main__':
    main(int(sys.argv[1]))
