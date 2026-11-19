import html
import re
import sys
import time
from pathlib import Path

path, offset, phrase = sys.argv[1:]
deadline = time.monotonic() + 15
while time.monotonic() < deadline:
    text = Path(path).read_bytes()[int(offset):].decode("utf-8", "replace")
    entries = re.findall(r"SPEECH DISPATCHER: Speaking '(<speak>.*?</speak>)'", text, re.S)
    for item in entries:
        utterance = " ".join(html.unescape(re.sub(r"<[^>]+>", "", item)).split())
        if phrase.casefold() in utterance.casefold():
            print(phrase, ":", utterance[:240], flush=True)
            raise SystemExit(0)
    time.sleep(0.25)
raise SystemExit(f"Orca utterance missing: {phrase}")
