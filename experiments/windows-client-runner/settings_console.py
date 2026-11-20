"""Drive only the disposable Windows Settings test desktop through virtual USB input."""
import argparse
from collections import defaultdict
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

OCR_ENV = {**os.environ, "OMP_THREAD_LIMIT": "1", "OMP_NUM_THREADS": "1"}
# Text masks matched in the 1280x800 Win11 26100 picker from runs
# 44664522433, 44672323214 and 44676418431. Dark text is extracted
# independently of its light selection background; the button text is white.
PICKER_TEXT_MASKS = {
    "title": ((438, 174, 735, 201), "ef693f449fd86fd347dbbb967ce91b3ea40c84d3df127440e252c75d4286fcf6"),
    "xdnote": ((483, 476, 641, 503), "5678248edc737b6bb8a0d54410514e17bbe8919853858511424df86603f1d560"),
    "confirm": ((707, 609, 780, 630), "0f351eca0944dffbc381480425a831eec378418967933c2f66d5c4f011ffedea"),
}


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


def load_frame(path):
    with path.open("rb") as raw:
        if raw.readline().strip() != b"P6":
            raise RuntimeError("Expected QMP PPM screenshot")
        width, height = map(int, raw.readline().split())
        if raw.readline().strip() != b"255":
            raise RuntimeError("Expected 8-bit QMP desktop pixels")
        pixels = raw.read()
    if not (900 <= width <= 1920 and 650 <= height <= 1200):
        raise RuntimeError(f"Unexpected test desktop: {width}x{height}")
    if len(pixels) != width * height * 3:
        raise RuntimeError("Incomplete QMP screenshot")
    return (width, height), pixels


def read_text(path):
    tsv = subprocess.check_output(["tesseract", str(path), "stdout", "-l", "eng", "--psm", "11", "tsv"],
                                  text=True, stderr=subprocess.DEVNULL, timeout=24, env=OCR_ENV)
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
    return spans


def match_picker(size, pixels):
    if size != (1280, 800):
        return {}
    matches = {}
    for name, (rect, digest) in PICKER_TEXT_MASKS.items():
        x0, y0, x1, y1 = rect
        mask = bytearray()
        for y in range(y0, y1):
            for x in range(x0, x1):
                offset = 3 * (y * size[0] + x)
                r, g, b = pixels[offset:offset + 3]
                mask.append(int(r > 155 and g > 190 and b > 200) if name == "confirm" else int(max(r, g, b) < 100))
        if hashlib.sha256(mask).hexdigest() == digest:
            matches[name] = ((x0 + x1) // 2, (y0 + y1) // 2)
    return matches


def point(spans, label, valid=lambda _: True):
    return next((p for text, p in spans if text == label and valid(p)), None)


def confirm_crop(frame, size, heading):
    """Recover the high-contrast text inside the Settings dialog's blue button."""
    with frame.open("rb") as raw:
        for _ in range(3):
            raw.readline()
        pixels = raw.read()
    width, height = size
    assert len(pixels) == width * height * 3
    hx, hy = heading
    left, right = max(0, hx - 25), min(width, hx + 265)
    top, bottom = hy + 230, min(height, hy + 625)
    bands = []
    for y in range(top, bottom):
        row = []
        for x in range(left, right):
            offset = 3 * (y * width + x)
            r, g, b = pixels[offset:offset + 3]
            if r <= 22 and 82 <= g <= 145 and 169 <= b <= 235:
                row.append(x)
        if len(row) >= 120 and 140 <= row[-1] - row[0] <= 245:
            if not bands or bands[-1][-1][0] != y - 1:
                bands.append([])
            bands[-1].append((y, row[0], row[-1]))
    for band in bands:
        if not 20 <= len(band) <= 55:
            continue
        y0, y1 = band[0][0], band[-1][0]
        x0 = min(r[1] for r in band)
        x1 = max(r[2] for r in band)
        if not 145 <= x1 - x0 <= 245:
            continue
        # Use white letters as black ink and exclude the rounded frame border.
        crop = bytearray()
        for y in range(y0 + 3, y1 - 2):
            for x in range(x0 + 8, x1 - 7):
                i = 3 * (y * width + x)
                r, g, b = pixels[i:i + 3]
                crop.append(0 if r > 155 and g > 190 and b > 200 else 255)
        return ((x0 + x1) // 2, (y0 + y1) // 2), f"P5\n{x1 - x0 - 15} {y1 - y0 - 5}\n255\n".encode() + crop
    return None


def confirm_point(frame, size, heading, spans):
    hx, hy = heading
    visible = point(spans, "set default", lambda p: hy + 220 < p[1] < hy + 650 and abs(p[0] - hx) < 350)
    if visible:
        return visible
    candidate = confirm_crop(frame, size, heading)
    if candidate:
        position, image = candidate
        label = subprocess.check_output(["tesseract", "stdin", "stdout", "-l", "eng", "--psm", "7"],
                                        input=image, stderr=subprocess.DEVNULL, timeout=18, env=OCR_ENV)
        if normalize(label.decode()) == "set default":
            return position
    return None


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
        if time.monotonic() - last_action < 2:
            time.sleep(0.6)
            continue
        console.call("screendump", filename=str(frame))
        size, pixels = load_frame(frame)
        known = match_picker(size, pixels)
        if "title" in known and "confirm" in known and confirms < 3:
            if selected:
                record("set-default", known["confirm"])
                console.click(known["confirm"], size)
                selected = None
                confirms += 1
                last_action = time.monotonic()
                continue
            if "xdnote" in known:
                record("select-xdnote", known["xdnote"])
                console.click(known["xdnote"], size)
                selected = known["xdnote"]
                last_action = time.monotonic()
                continue
        try:
            spans = read_text(frame)
        except subprocess.TimeoutExpired:
            print("OCR took too long while the guest was busy; capturing a new frame", flush=True)
            time.sleep(2)
            continue
        heading = point(spans, "select a default app for md files")
        if not heading:
            heading = point(spans, "select a default app for md")
        if heading:
            hx, hy = heading
            choice = point(spans, "xdnote markdown editor", lambda p: hy + 45 < p[1] < hy + 500 and abs(p[0] - hx) < 255)
            if choice and confirms < 3:
                if not selected or any(abs(a - b) > 8 for a, b in zip(selected, choice)):
                    record("select-xdnote", choice)
                    console.click(choice, size)
                    selected = choice
                else:
                    try:
                        confirm = confirm_point(frame, size, heading, spans)
                    except subprocess.TimeoutExpired:
                        print("Button OCR took too long; retrying on the next frame", flush=True)
                        time.sleep(2)
                        continue
                    if not confirm:
                        time.sleep(1)
                        continue
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
                    row_bounds = lambda p: rx < p[0] < rx + 400 and ry + 6 < p[1] < ry + 100
                    handler = point(spans, "notepad", row_bounds) or point(spans, "choose a default", row_bounds)
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
