import sys
import time
import pyatspi
from gi.repository import Atspi

def descendants(obj):
    yield obj
    for child in obj:
        if child is not None:
            yield from descendants(child)

desktop = pyatspi.Registry.getDesktop(0)
Atspi.set_timeout(2500, 8000)
if sys.argv[1] == "closed":
    for _ in range(30):
        if not any((node.name or "").startswith("Reader-Check.md") for app in desktop
                   if app is not None and app.name.lower() == "xdnote"
                   for node in descendants(app)):
            print("Owned reader window disappeared from AT-SPI")
            raise SystemExit(0)
        time.sleep(0.2)
    raise SystemExit("Owned reader window still present after close")

reader = None
nodes = []
for _ in range(20):
    desktop.clear_cache()
    apps = [entry for entry in desktop if entry is not None and entry.name.lower() == "xdnote"]
    for app in apps:
        app.clear_cache()
    nodes = [node for app in apps for node in descendants(app)]
    reader = next((node for node in nodes if node.name == "Markdown 阅读内容"), None)
    if reader is not None:
        break
    time.sleep(0.35)
if reader is None:
    print(f"xdnote AT-SPI applications={len(apps)} nodes={len(nodes)}")
    for node in nodes[:60]:
        print(f"{node.getRoleName()}: {node.name!r}, children={node.childCount}")
    raise SystemExit("Markdown reader missing from AT-SPI")
text = reader.queryText()
marker = "Amber target reading sample"
position = text.getText(0, text.characterCount).find(marker)
if position < 0:
    raise SystemExit("Target missing from AX text")
coordinate_system = pyatspi.DESKTOP_COORDS
char = text.getCharacterExtents(position, coordinate_system)
bounds = reader.queryComponent().getExtents(coordinate_system)
visible = bounds.y <= char[1] < bounds.y + bounds.height
print(f"AX extent of target visible: {visible}, target y={char[1]}, viewport y={bounds.y}+{bounds.height}")
if visible != (sys.argv[1] == "visible"):
    raise SystemExit("Unexpected target viewport position")
