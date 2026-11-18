#!/usr/bin/env bash
set -euo pipefail

evidence="$PWD/build/evidence/reader-orca"
mkdir -p "$evidence/home"
export XDG_CONFIG_HOME="$evidence/home/config"
export XDG_CACHE_HOME="$evidence/home/cache"
export XDG_DATA_HOME="$evidence/home/data"
export QT_QPA_PLATFORM=xcb
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export NO_AT_BRIDGE=0
export LC_ALL=C.UTF-8

body="Blue orchard reading sample"
link="Follow the cedar heading"
fixture="$evidence/Reader-Check.md"
cat > "$fixture" <<'DOC'
# Native reading acceptance

Blue orchard reading sample.

[Follow the cedar heading](#cedar-anchor)

DOC
for n in $(seq 1 40); do printf 'Filler paragraph %s.\n\n' "$n"; done >> "$fixture"
printf '## Cedar anchor\n\nAmber target reading sample.\n' >> "$fixture"
original="$(sha256sum "$fixture")"
owned_pids=()
finish() {
    if (("${#owned_pids[@]}")); then kill "${owned_pids[@]}" 2>/dev/null || true; fi
}
trap finish EXIT

orca --version
openbox > "$evidence/openbox.log" 2>&1 & owned_pids+=("$!")
pulseaudio --start --exit-idle-time=-1
pactl load-module module-null-sink sink_name=xdnote_acceptance
pactl set-default-sink xdnote_acceptance
gsettings set org.gnome.desktop.interface toolkit-accessibility true
orca --enable=speech --disable=braille --debug-file="$evidence/orca.log" > "$evidence/orca-console.log" 2>&1 &
owned_pids+=("$!")
screen_reader=${owned_pids[-1]}
sleep 2
kill -0 "$screen_reader"
python scripts/desktop.py run --profile default --build-dir build/desktop-admission \
    --qt-prefix build/Qt/6.8.2/gcc_64 --file "$fixture" > "$evidence/app.log" 2>&1 &
owned_pids+=("$!")
window="$(timeout 30s xdotool search --sync --onlyvisible --name '^Reader-Check[.]md.*xdnote$' | head -1)"
xdotool windowactivate --sync "$window"
xdotool mousemove --window "$window" 120 215 click 1
xdotool key --clearmodifiers ctrl+Home
sleep 1
/usr/bin/python3 scripts/reader-acceptance/orca-target.py hidden
xdotool key --clearmodifiers KP_Add
sleep 6
read_end="$(wc -c < "$evidence/orca.log")"
xdotool key --clearmodifiers ctrl+a
xdotool key --clearmodifiers Insert+Shift+Up
sleep 1
selection_end="$(wc -c < "$evidence/orca.log")"
xdotool key --clearmodifiers ctrl+Home
sleep 0.5
focus_start="$(wc -c < "$evidence/orca.log")"
xdotool key --clearmodifiers Tab
sleep 1
focus_end="$(wc -c < "$evidence/orca.log")"
xdotool key --clearmodifiers Return
sleep 1
/usr/bin/python3 scripts/reader-acceptance/orca-target.py visible
xdotool key --clearmodifiers ctrl+w
sleep 1
if xdotool search --onlyvisible --name '^Reader-Check[.]md.*xdnote$'; then
    echo 'Owned document window still visible after closing'
    exit 1
fi
test "$original" = "$(sha256sum "$fixture")"
python - "$evidence/orca.log" "$read_end" "$selection_end" "$focus_start" "$focus_end" <<'PYCODE'
import html
import re
import sys
from pathlib import Path

log = Path(sys.argv[1]).read_bytes()
read_end, selection_end, focus_start, focus_end = map(int, sys.argv[2:])

def speech(data):
    entries = re.findall(r"SPEECH DISPATCHER: Speaking '(<speak>.*?</speak>)'", data.decode("utf-8", "replace"), re.S)
    return [" ".join(html.unescape(re.sub(r"<[^>]+>", "", item)).split()) for item in entries]

checks = [
    ("Body reading", log[:read_end], "Blue orchard reading sample"),
    ("Text selection", log[read_end:selection_end], "Selected text is:"),
    ("Link focus", log[focus_start:focus_end], "Follow the cedar heading"),
]
for label, part, marker in checks:
    matches = [utterance for utterance in speech(part) if marker.casefold() in utterance.casefold()]
    if not matches:
        print(label, "missing", speech(part)[-8:])
        sys.exit(1)
    print(label, matches[0][:220])
PYCODE
