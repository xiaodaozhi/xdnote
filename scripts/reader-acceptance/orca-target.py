import sys
import time
import pyatspi

def descendants(obj):
    yield obj
    for child in obj:
        if child is not None:
            yield from descendants(child)

desktop = pyatspi.Registry.getDesktop(0)
if sys.argv[1] == "closed":
    for _ in range(30):
        if not any(node.name.startswith("Reader-Check.md") for app in desktop
                   if app is not None and app.name.lower() == "xdnote"
                   for node in descendants(app)):
            print("Owned reader window disappeared from AT-SPI")
            raise SystemExit(0)
        time.sleep(0.2)
    raise SystemExit("Owned reader window still present after close")

app = next((entry for entry in desktop if entry.name.lower() == "xdnote"), None)
if app is None:
    raise SystemExit("xdnote missing from AT-SPI desktop")
nodes = list(descendants(app))
reader = next((node for node in nodes if node.name == "Markdown 阅读内容"), None)
if reader is None:
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
