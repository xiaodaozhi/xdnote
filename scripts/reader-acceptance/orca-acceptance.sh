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
xdotool key --clearmodifiers KP_Add
sleep 5
xdotool key --clearmodifiers ctrl+a
xdotool key --clearmodifiers Insert+Shift+Up
sleep 1
xdotool key --clearmodifiers ctrl+Home
xdotool key --clearmodifiers Tab KP_Enter
sleep 1
xdotool key --clearmodifiers Return
sleep 1
xdotool key --clearmodifiers KP_Add
sleep 5
xdotool key --clearmodifiers ctrl+w
sleep 1
if xdotool search --onlyvisible --name '^Reader-Check[.]md.*xdnote$'; then
    echo 'Owned document window still visible after closing'
    exit 1
fi
test "$original" = "$(sha256sum "$fixture")"
python - "$evidence/orca.log" "$body" "$link" <<'PY'
import re
import sys
from pathlib import Path

log = Path(sys.argv[1]).read_text(errors="replace")
spoken = [line for line in log.splitlines()
          if re.search(r"speech\s+output|speaking|\bspeak(?:ing)?\s*[:(]|speech(?:\s*[:.].*)dispat", line, re.I)]
for marker in [*sys.argv[2:], "Amber target reading sample"]:
    matches = [line.strip()[:520] for line in spoken if marker.casefold() in line.casefold()]
    print(f"{marker}: {len(matches)} actual Orca speech log entries")
    for match in matches[:3]:
        print(match)
    if not matches:
        print("Probe context", [line.strip()[:260] for line in log.splitlines()
                                if marker.casefold() in line.casefold()][:5])
        sys.exit(1)
PY
