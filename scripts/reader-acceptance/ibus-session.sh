#!/usr/bin/env bash
set -euo pipefail
evidence="$PWD/build/evidence/ibus-x11"
mkdir -p "$evidence/workspace" "$evidence/home"
export HOME="$evidence/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR="$(mktemp -d /tmp/xdnote-ibus.XXXXXX)"
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=xcb QT_IM_MODULE=ibus QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export GTK_IM_MODULE=ibus XMODIFIERS=@im=ibus NO_AT_BRIDGE=0
fixture="$evidence/workspace/IME-Check.md"
printf '# Input acceptance\n\nSeed: ' > "$fixture"
before="$(sha256sum "$fixture")"
owned=()
finish() {
    if (( ${#owned[@]} )); then kill "${owned[@]}" 2>/dev/null || true; fi
    rmdir "$XDG_RUNTIME_DIR" 2>/dev/null || true
}
trap finish EXIT
openbox --sm-disable > "$evidence/openbox.log" 2>&1 & owned+=("$!")
ibus-daemon --replace > "$evidence/ibus.log" 2>&1 & owned+=("$!")
for attempt in {1..30}; do
    IBUS_ADDRESS="$(ibus address)"
    [[ "$IBUS_ADDRESS" == unix:* ]] && break
    sleep 0.3
done
[[ "$IBUS_ADDRESS" == unix:* ]]
export IBUS_ADDRESS
ibus version
engine="$(/usr/bin/python3 -c 'from xml.etree import ElementTree as E; print(E.parse("/usr/share/ibus/component/libpinyin.xml").findtext(".//engine/name"))')"
ibus list-engine > "$evidence/engines.log"
test -n "$engine"
dbus-monitor --address "$IBUS_ADDRESS" "type='signal',member='UpdatePreeditText'" "type='signal',member='CommitText'" > "$evidence/ime-bus.log" 2>&1 & owned+=("$!")
launch=(python scripts/desktop.py run --profile default --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/gcc_64)
"${launch[@]}" > "$evidence/app.log" 2>&1 & owned+=("$!")
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py directory "$evidence/workspace"
"${launch[@]}" --file "$fixture" >> "$evidence/app.log" 2>&1
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py edit
ibus engine "$engine"
test "$(ibus engine)" = "$engine"
xdotool key --clearmodifiers ctrl+End n i h a o
for attempt in {1..30}; do
    grep -q 'member=UpdatePreeditText' "$evidence/ime-bus.log" && break
    sleep 0.25
done
grep -q 'member=UpdatePreeditText' "$evidence/ime-bus.log"
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py preedit
xdotool key --clearmodifiers space
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py commit
ibus engine xkb:us::eng
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py transfer
test "$before" = "$(sha256sum "$fixture")"
echo "PASS: IBus Pinyin composition, commit, workspace transfer, undo, redo, selection, and unchanged disk fixture"
