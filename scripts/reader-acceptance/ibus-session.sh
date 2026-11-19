#!/usr/bin/env bash
set -euo pipefail
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_ENVIRONMENT:-}" != github-hosted ||
      -z "${DISPLAY:-}" || -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    echo 'Run the IBus acceptance in the disposable CI desktop session' >&2
    exit 2
fi
evidence="$PWD/build/evidence/ibus-x11"
mkdir -p "$evidence/workspace" "$evidence/home"
export HOME="$evidence/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR="$(mktemp -d /tmp/xdnote-ibus.XXXXXX)"
export LC_ALL=C.UTF-8 QT_QPA_PLATFORM=xcb QT_IM_MODULE=ibus QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export GTK_IM_MODULE=ibus XMODIFIERS=@im=ibus NO_AT_BRIDGE=0
dbus-update-activation-environment HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR DISPLAY
gsettings set org.freedesktop.ibus.general use-system-keyboard-layout true
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
engine=libpinyin
ibus list-engine > "$evidence/engines.log"
grep -F "  $engine - " "$evidence/engines.log"
bus_files=("$XDG_CONFIG_HOME"/ibus/bus/*)
[[ ${#bus_files[@]} -eq 1 && -f "${bus_files[0]}" ]]
export IBUS_ADDRESS_FILE="${bus_files[0]}"
dbus-monitor --address "$IBUS_ADDRESS" "type='signal',interface='org.freedesktop.IBus.InputContext'" > "$evidence/ime-bus.log" 2>&1 & owned+=("$!")
launch=(python scripts/desktop.py run --profile default --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/gcc_64)
"${launch[@]}" > "$evidence/app.log" 2>&1 & owned+=("$!")
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py directory "$evidence/workspace"
"${launch[@]}" --file "$fixture" >> "$evidence/app.log" 2>&1
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py edit
ibus engine "$engine"
active_engine="$(ibus engine)"
[[ "$active_engine" == "$engine" ]] || { echo "IBus selected $active_engine instead of $engine" >&2; exit 1; }
xdotool key --clearmodifiers ctrl+End n i h a o
for attempt in {1..30}; do
    grep -Fq 'string "你好"' "$evidence/ime-bus.log" && break
    sleep 0.25
done
if ! grep -Fq 'string "你好"' "$evidence/ime-bus.log" || grep -q 'member=CommitText' "$evidence/ime-bus.log"; then
    echo 'IBus did not expose the expected uncommitted candidate' >&2
    exit 1
fi
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py preedit
xdotool key --clearmodifiers space
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py commit
grep -q 'member=CommitText' "$evidence/ime-bus.log"
ibus engine xkb:us::eng
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py transfer
test "$before" = "$(sha256sum "$fixture")"
echo "PASS: IBus Pinyin composition, commit, workspace transfer, undo, redo, selection, and unchanged disk fixture"
/usr/bin/python3 scripts/reader-acceptance/ibus-x11.py accessible-write
test "$before" = "$(sha256sum "$fixture")"
