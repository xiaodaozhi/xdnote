"""Exercise only the owned Quick editor through the Linux accessibility bus."""

import sys
import time

import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi
import pyatspi


def descendants(node, depth=0):
    if depth == 8:
        return
    for child in node:
        if child is not None:
            yield child
            yield from descendants(child, depth + 1)


def find_editor(process_id):
    desktop = pyatspi.Registry.getDesktop(0)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        desktop.clear_cache()
        for app in desktop:
            if app is None or app.get_process_id() != process_id:
                continue
            app.clear_cache()
            for window in descendants(app):
                if window.name == 'xdnote AT-SPI research fixture':
                    for editor in descendants(window):
                        if editor.name == 'Document':
                            return editor
        time.sleep(0.15)
    raise RuntimeError('The owned editor did not appear on the AT-SPI bus')


def require_text(editor, expected):
    editor.clear_cache()
    text = editor.queryText()
    actual = text.getText(0, text.characterCount)
    if actual != expected:
        raise ValueError(f'AT-SPI exposes {actual!r}, expected {expected!r}')
    return text


def wait_for_command(expected):
    if sys.stdin.readline().strip() != expected:
        raise RuntimeError(f'Fixture did not request {expected}')


def main(process_id):
    Atspi.set_timeout(2000, 5000)
    editor = find_editor(process_id)
    require_text(editor, 'old document')
    print('READY', flush=True)

    wait_for_command('CHECK')
    text = require_text(editor, 'new document!')
    count = text.getNSelections()
    bounds = tuple(text.getSelection(0)) if count else None
    if count != 1 or bounds != (0, 3):
        raise ValueError(f'AT-SPI reported {count} selections, first range {bounds!r}')
    print('SELECTION_CHECKED', flush=True)

    wait_for_command('READONLY')
    editor.queryEditableText().setTextContents('rejected read-only write')
    require_text(editor, 'new document!')
    print('READ_ONLY_CHECKED', flush=True)

    wait_for_command('WRITE')
    if not editor.queryEditableText().setTextContents('written through AT-SPI'):
        raise ValueError('AT-SPI could not replace the editable content')
    require_text(editor, 'written through AT-SPI')


if __name__ == '__main__':
    main(int(sys.argv[1]))
