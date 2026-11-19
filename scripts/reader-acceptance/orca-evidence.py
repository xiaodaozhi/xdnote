"""Check fixture speech and viewport state in an isolated Orca session."""

import argparse
import html
import re
import time
from pathlib import Path

SPEECH_ENTRY = re.compile(r"SPEECH DISPATCHER: Speaking '(<speak>.*?</speak>)'", re.S)
TARGET = "Amber target reading sample"


def wait_for_speech(log, phase, offset, phrase):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        with log.open("rb") as stream:
            stream.seek(offset)
            text = stream.read().decode("utf-8", "replace")
        for entry in SPEECH_ENTRY.findall(text):
            spoken = " ".join(html.unescape(re.sub(r"<[^>]+>", "", entry)).split())
            if phrase.casefold() in spoken.casefold():
                print(f"{phase}: {phrase}", flush=True)
                return
        time.sleep(0.25)
    raise SystemExit(f"Orca utterance missing in {phase}: {phrase}")


def descendants(node):
    yield node
    for child in node:
        if child is not None:
            yield from descendants(child)


def fixture_windows(desktop, title):
    desktop.clear_cache()
    for app in desktop:
        if app is not None and (app.name or "").casefold() == "xdnote":
            app.clear_cache()
            for window in app:
                if window is not None and (window.name or "").startswith(title):
                    yield window


def check_window(expected, title):
    from gi.repository import Atspi
    import pyatspi

    Atspi.set_timeout(2500, 8000)
    desktop = pyatspi.Registry.getDesktop(0)
    for _ in range(30):
        windows = list(fixture_windows(desktop, title))
        if expected == "closed":
            if not windows:
                print("Fixture window disappeared from AT-SPI")
                return
        elif len(windows) == 1:
            reader = next(
                (node for node in descendants(windows[0]) if node.name == "Markdown 阅读内容"),
                None,
            )
            if reader is not None:
                text = reader.queryText()
                position = text.getText(0, text.characterCount).find(TARGET)
                if position >= 0:
                    coordinates = pyatspi.DESKTOP_COORDS
                    _, y, _, height = text.getCharacterExtents(position, coordinates)
                    bounds = reader.queryComponent().getExtents(coordinates)
                    visible = height > 0 and bounds.y <= y < bounds.y + bounds.height
                    if visible == (expected == "visible"):
                        print(f"Target {expected}: y={y}, viewport y={bounds.y}+{bounds.height}")
                        return
        time.sleep(0.25)
    raise SystemExit(f"Fixture did not reach AT-SPI state: {expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    speech = commands.add_parser("speech")
    speech.add_argument("log", type=Path)
    speech.add_argument("phase")
    speech.add_argument("offset", type=int)
    speech.add_argument("phrase")
    window = commands.add_parser("window")
    window.add_argument("expected", choices=("hidden", "visible", "closed"))
    window.add_argument("title")
    args = parser.parse_args()
    if args.command == "speech":
        wait_for_speech(args.log, args.phase, args.offset, args.phrase)
    else:
        check_window(args.expected, args.title)


if __name__ == "__main__":
    main()
