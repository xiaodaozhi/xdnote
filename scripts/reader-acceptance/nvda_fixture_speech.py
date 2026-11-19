"""Record only known fixture markers from speech passed to NVDA's synthesizer."""

import re
from pathlib import Path

import api
import globalPluginHandler
from synthDriverHandler import pre_synthSpeak


ROOT = Path(__file__).resolve().parents[2]
MARKERS = {
    "opening": "opening heading",
    "body": "opening reading marker",
    "link": "jump to the distant target",
    "target": "distant target heading",
    "continuation": "the destination paragraph is now being read",
    "filler": "intervening filler",
    "source": "markdown 源文",
}


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        self.fixture_pid = int((ROOT / "fixture-pid.txt").read_text(encoding="ascii"))
        pre_synthSpeak.register(self.capture)
        (ROOT / "ready").touch()

    def terminate(self):
        pre_synthSpeak.unregister(self.capture)

    def capture(self, speechSequence):
        foreground = api.getForegroundObject()
        if not foreground or foreground.processID != self.fixture_pid:
            return
        text = " ".join(part for part in speechSequence if isinstance(part, str)).casefold()
        markers = [name for name, phrase in MARKERS.items() if phrase in text]
        if re.search(r"\bselected\b", text) and not re.search(r"\bnot selected\b", text):
            markers.append("selected")
        if not markers:
            return
        phase = (ROOT / "current-phase.txt").read_text(encoding="ascii").strip()
        with (ROOT.parent / "speech-markers.txt").open("a", encoding="utf-8") as stream:
            stream.write(f"{phase}: {', '.join(markers)}\n")
