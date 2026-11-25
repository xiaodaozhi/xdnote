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
UNDONE = 'Cedar arch previous draft'


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

    def hear(phase, expected, offset):
        if xdotool('getactivewindow') != window:
            raise RuntimeError('The isolated fixture lost focus')
        text = require_text(editor, expected)
        length = text.characterCount
        if text.getNSelections():
            if not text.removeSelection(0):
                raise RuntimeError('Could not clear the previous fixture selection')
            time.sleep(0.5)
        # Orca can speak the new text either on replacement or on selection.
        if not text.addSelection(0, length) or tuple(text.getSelection(0)) != (0, length):
            raise RuntimeError('Could not select the fixture text for Orca')
        subprocess.run(
            [sys.executable, str(speech_probe), 'speech', str(log), phase, str(offset), expected],
            stdout=sys.stderr, check=True, timeout=17,
        )
        require_text(editor, expected)

    hear('old-document', ORIGINAL, log.stat().st_size)
    before_replacement = log.stat().st_size
    print('OLD_SPOKEN', flush=True)

    wait_for_command('READ_ONLY')
    hear('read-only-replacement', REPLACEMENT, before_replacement)
    try:
        editor.queryEditableText()
    except NotImplementedError:
        pass
    else:
        raise RuntimeError('The read-only editor still exposed the AT-SPI write interface')
    require_text(editor, REPLACEMENT)
    print('REPLACEMENT_SPOKEN', flush=True)

    wait_for_command('WRITE')
    editor.clear_cache()
    before_write = log.stat().st_size
    if not editor.queryEditableText().setTextContents(WRITTEN):
        raise RuntimeError('The controlled AT-SPI client could not set the editable text')
    hear('after-external-write', WRITTEN, before_write)
    before_undo = log.stat().st_size
    print('WRITE_SPOKEN', flush=True)

    wait_for_command('UNDO')
    hear('after-two-undos', UNDONE, before_undo)
    print('UNDO_SPOKEN', flush=True)


if __name__ == '__main__':
    main(int(sys.argv[1]))
