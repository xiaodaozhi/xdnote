"""Read real xdnote editor state through AT-SPI after IBus and X11 input."""
import subprocess
import sys
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi
import pyatspi

ORIGINAL = "# Input acceptance\n\nSeed: "
COMMITTED = ORIGINAL + "你好"
Atspi.set_timeout(2000, 5000)

def input_event(*args):
    return subprocess.check_output(["xdotool", *args], text=True, timeout=10).strip()

def walk(node, depth=0):
    if depth == 15:
        return
    yield node
    for child in node:
        if child is not None:
            yield from walk(child, depth + 1)

def wait_for(predicate, description):
    desktop = pyatspi.Registry.getDesktop(0)
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        desktop.clear_cache()
        for app in desktop:
            if app is None or (app.name or "").casefold() != "xdnote":
                continue
            app.clear_cache()
            for node in walk(app):
                try:
                    if predicate(node):
                        return node
                except (LookupError, NotImplementedError):
                    pass
        time.sleep(0.25)
    raise ValueError(description)

def visible(node):
    return node.getState().contains(pyatspi.STATE_SHOWING)

def show(name):
    return wait_for(lambda item: item.name == name and visible(item), f"Not visible: {name}")

def click(node):
    bounds = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
    if min(bounds.width, bounds.height) < 1:
        raise ValueError("The owned control has no clickable bounds")
    input_event("mousemove", str(bounds.x + bounds.width // 2), str(bounds.y + bounds.height // 2), "click", "1")

def keys(*names):
    input_event("key", "--clearmodifiers", *names)

def document(expected, focus=True):
    def found(node):
        if node.name != "Markdown 源文" or not visible(node):
            return False
        if focus and not node.getState().contains(pyatspi.STATE_FOCUSED):
            return False
        text = node.queryText()
        return text.getText(0, text.characterCount) == expected
    return wait_for(found, f"Expected focused editor with {expected!r}")

phase = sys.argv[1]
if phase == "directory":
    click(show("打开文件夹"))
    show("打开工作区目录")
    keys("ctrl+l")
    input_event("type", "--clearmodifiers", sys.argv[2])
    keys("Return")
    wait_for(lambda node: visible(node) and (node.name or "").startswith("workspace — xdnote"),
             "The selected directory did not open as the active workspace")
elif phase == "edit":
    click(show("编辑"))
    click(document(ORIGINAL, focus=False))
    document(ORIGINAL)
elif phase == "preedit":
    # The editor's accessible snapshot is withheld while Scintilla has tentative edits.
    document("")
    print("The real IBus candidate is composing and the document snapshot is withheld")
elif phase == "commit":
    document(COMMITTED)
    print("The active editor contains the real Pinyin commit")
elif phase == "transfer":
    click(show("在工作区打开"))
    editor = document(COMMITTED)
    while editor.parent:
        editor = editor.parent
        if (editor.name or "").startswith("workspace — xdnote"):
            break
    else:
        raise ValueError("The focused editor did not transfer into the workspace")
    keys("ctrl+z")
    document(ORIGINAL)
    keys("ctrl+shift+z")
    document(COMMITTED)
    keys("ctrl+End", "shift+Left", "shift+Left")
    text = document(COMMITTED).queryText()
    start, end = text.getSelection(0)
    if text.getText(start, end) != "你好":
        raise ValueError("Editor did not retain a two-character Chinese selection")
    keys("ctrl+z")
    document(ORIGINAL)
    print("Workspace editor retained selection, undo, and redo")
elif phase == "accessible-write":
    replacement = "# AT-SPI write acceptance\n\n独立接口写入"
    if not document(ORIGINAL).queryEditableText().setTextContents(replacement):
        raise ValueError("The external AT-SPI client could not replace the editable document")
    document(replacement)
    keys("ctrl+z")
    document(ORIGINAL)
    print("PASS: an external AT-SPI whole-value write was undoable in one step")
else:
    raise SystemExit("Unknown fixture phase")
