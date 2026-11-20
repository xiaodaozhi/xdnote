"""Drive only the disposable Windows Settings test desktop through virtual USB input."""
import argparse
from collections import defaultdict
import csv
import io
import json
from pathlib import Path
import re
import socket
import subprocess
import time


class Console:
    def __init__(self, root):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(12)
        self.socket.connect(str(root / "qmp"))
        self.stream = self.socket.makefile("rwb")
        greeting = json.loads(self.stream.readline())
        if "QMP" not in greeting:
            raise RuntimeError("Missing QMP greeting")
        self.call("qmp_capabilities")

    def call(self, name, **arguments):
        self.stream.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\n")
        self.stream.flush()
        while reply := self.stream.readline():
            value = json.loads(reply)
            if "error" in value:
                raise RuntimeError(value["error"])
            if "return" in value:
                return value["return"]
        raise RuntimeError("QMP connection closed")

    def click(self, point, size):
        x, y = point
        w, h = size
        self.call("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(coord * 32767 / (length - 1))}}
            for axis, coord, length in (("x", x, w), ("y", y, h))
        ])
        time.sleep(0.18)
        for down in (True, False):
            self.call("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
            time.sleep(0.12)

    def escape(self):
        self.call("send-key", keys=[{"type": "qcode", "data": "esc"}], **{"hold-time": 100})


def normalize(text):
    return " ".join(re.findall(r"[a-z0-9]+", text.lower()))


def read_screen(console, path):
    console.call("screendump", filename=str(path))
    with path.open("rb") as raw:
        if raw.readline().strip() != b"P6":
            raise RuntimeError("Expected QMP PPM screenshot")
        width, height = map(int, raw.readline().split())
    if not (900 <= width <= 1920 and 650 <= height <= 1200):
        raise RuntimeError(f"Unexpected test desktop: {width}x{height}")
    tsv = subprocess.check_output(["tesseract", str(path), "stdout", "-l", "eng", "--psm", "11", "tsv"],
                                  text=True, stderr=subprocess.DEVNULL, timeout=8)
    rows = defaultdict(list)
    for word in csv.DictReader(io.StringIO(tsv), delimiter="\t", quoting=csv.QUOTE_NONE):
        if word["text"].strip():
            key = tuple(word[k] for k in ("block_num", "par_num", "line_num"))
            rows[key].append(word)
    spans = []
    for words in rows.values():
        for start in range(len(words)):
            for end in range(start + 1, len(words) + 1):
                line = words[start:end]
                label = normalize(" ".join(item["text"] for item in line))
                first, last = line[0], line[-1]
                left, right = int(first["left"]), int(last["left"]) + int(last["width"])
                y = int(first["top"]) + int(first["height"]) // 2
                spans.append((label, ((left + right) // 2, y)))
    return (width, height), spans


def point(spans, label, valid=lambda _: True):
    return next((p for text, p in spans if text == label and valid(p)), None)


def run(root):
    console = Console(root)
    evidence = root / "evidence"
    serial = root / "serial.log"
    frame = evidence / "settings-latest.ppm"
    start = "XDNOTE_VM_STAGE Windows Settings console input requested"
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline and start not in serial.read_text(errors="replace"):
        if "XDNOTE_VM_RESULT " in serial.read_text(errors="replace"):
            return
        time.sleep(1)
    if start not in serial.read_text(errors="replace"):
        raise RuntimeError("Guest did not request Settings console input")

    history = []
    deadline = time.monotonic() + 150
    selected = None
    last_action = 0
    confirms = 0

    def record(action, location=None):
        history.append({"action": action, "point": location})
        (evidence / "settings-input.json").write_text(json.dumps(history, indent=2) + "\n")
        (evidence / f"settings-{len(history):02d}-{action}.ppm").write_bytes(frame.read_bytes())

    while time.monotonic() < deadline:
        serial_text = serial.read_text(errors="replace")
        if "XDNOTE_VM_STAGE Windows explicit default read back: " in serial_text:
            return
        if "XDNOTE_VM_RESULT " in serial_text:
            return
        size, spans = read_screen(console, frame)
        if time.monotonic() - last_action < 2:
            time.sleep(0.6)
            continue
        heading = point(spans, "select a default app for md files")
        if not heading:
            heading = point(spans, "select a default app for md")
        if heading:
            hx, hy = heading
            choice = point(spans, "xdnote markdown editor", lambda p: hy + 45 < p[1] < hy + 500 and abs(p[0] - hx) < 255)
            confirm = point(spans, "set default", lambda p: hy + 220 < p[1] < hy + 650 and abs(p[0] - hx) < 350)
            if choice and confirm and confirms < 3:
                if selected != choice:
                    record("select-xdnote", choice)
                    console.click(choice, size)
                    selected = choice
                else:
                    record("set-default", confirm)
                    console.click(confirm, size)
                    selected = None
                    confirms += 1
                last_action = time.monotonic()
                continue
        selected = None
        start_search = (point(spans, "search for apps settings and documents")
                        or (point(spans, "pinned") and point(spans, "recommended") and point(spans, "all apps")))
        if start_search and len(history) < 15:
            record("dismiss-start")
            console.escape()
        else:
            page = point(spans, "default apps")
            title = point(spans, "xdnote", lambda p: page and abs(p[1] - page[1]) < 28 and p[0] > page[0])
            if page and title and len(history) < 15:
                row = point(spans, "md", lambda p: abs(p[0] - (page[0] - 160)) < 180 and page[1] + 110 < p[1] < page[1] + 450)
                if row:
                    rx, ry = row
                    handler = point(spans, "notepad", lambda p: rx < p[0] < rx + 400 and ry + 6 < p[1] < ry + 100)
                    if handler and confirms == 0:
                        record("open-md-row", handler)
                        console.click(handler, size)
                    else:
                        time.sleep(1)
                        continue
                else:
                    time.sleep(1)
                    continue
            else:
                time.sleep(1)
                continue
        last_action = time.monotonic()

    raise RuntimeError("Windows Settings console did not obtain a confirmed explicit default choice")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    run(parser.parse_args().directory.resolve())
