#!/usr/bin/env bash
set -euo pipefail
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_ENVIRONMENT:-}" != github-hosted ||
      -z "${DISPLAY:-}" || -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    echo 'Run the IBus acceptance in the disposable CI desktop session' >&2
    exit 2
fi
backend="${1:-x11}"
if [[ "$backend" != x11 && "$backend" != wayland ]]; then
    echo 'Choose x11 or wayland' >&2
    exit 2
fi
evidence="$PWD/build/evidence/ibus-$backend"
mkdir -p "$evidence/workspace" "$evidence/home"
export HOME="$evidence/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR="$(mktemp -d /tmp/xdnote-ibus.XXXXXX)"
export LC_ALL=C.UTF-8 QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 NO_AT_BRIDGE=0
dbus-update-activation-environment HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR DISPLAY
gsettings set org.freedesktop.ibus.general use-system-keyboard-layout true
fixture="$evidence/workspace/IME-Check.md"
printf '# Input acceptance\n\nSeed: ' > "$fixture"
before="$(sha256sum "$fixture")"
owned=()
finish() {
    if (( ${#owned[@]} )); then
        kill "${owned[@]}" 2>/dev/null || true
        for child in "${owned[@]}"; do wait "$child" 2>/dev/null || true; done
    fi
    rmdir "$XDG_RUNTIME_DIR" 2>/dev/null || true
}
trap finish EXIT
openbox --sm-disable > "$evidence/openbox.log" 2>&1 & owned+=("$!")

launch=(python scripts/desktop.py run --profile default --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/gcc_64)
if [[ "$backend" == wayland ]]; then
    unset IBUS_ADDRESS IBUS_ADDRESS_FILE
    gsettings set org.gnome.desktop.interface toolkit-accessibility true
    gsettings set org.gnome.desktop.input-sources sources "[('xkb', 'us'), ('ibus', 'libpinyin')]"
    gsettings set org.gnome.desktop.wm.keybindings switch-input-source "['<Super>space']"
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME
    LIBGL_ALWAYS_SOFTWARE=1 gnome-shell --nested --wayland > "$evidence/mutter.log" 2>&1 &
    compositor=$!
    owned+=("$compositor")
    unset WAYLAND_DISPLAY
    window=
    for attempt in {1..60}; do
        kill -0 "$compositor" || { echo 'GNOME Shell exited' >&2; exit 1; }
        for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
            if [[ -S "$socket" ]]; then export WAYLAND_DISPLAY="${socket##*/}"; break; fi
        done
        window="$(xdotool search --onlyvisible --pid "$compositor" 2>/dev/null | sed -n '1p' || true)"
        if [[ -z "$window" ]]; then
            window="$(xdotool search --onlyvisible --class 'gnome-shell|mutter' 2>/dev/null | sed -n '1p' || true)"
        fi
        [[ -n "$window" && -n "${WAYLAND_DISPLAY:-}" ]] && break
        sleep 0.5
    done
    [[ -n "$window" && -n "${WAYLAND_DISPLAY:-}" ]]
    export XDNOTE_TEST_INPUT_WINDOW="$window"
    xdotool windowactivate --sync "$window"
    launch=(env -u DISPLAY -u QT_IM_MODULE -u QT_IM_MODULES -u QT_WAYLAND_TEXT_INPUT_PROTOCOL
            QT_QPA_PLATFORM=wayland WAYLAND_DEBUG=client "${launch[@]}")
    gnome-shell --version
else
    export QT_QPA_PLATFORM=xcb QT_IM_MODULE=ibus GTK_IM_MODULE=ibus XMODIFIERS=@im=ibus
    ibus-daemon --replace > "$evidence/ibus.log" 2>&1 & owned+=("$!")
fi
for attempt in {1..30}; do
    IBUS_ADDRESS="$(ibus address)"
    [[ "$IBUS_ADDRESS" == unix:* ]] && break
    sleep 0.3
done
[[ "$IBUS_ADDRESS" == unix:* ]]
ibus version
engine=libpinyin
ibus list-engine > "$evidence/engines.log"
grep -F "  $engine - " "$evidence/engines.log"
if [[ "$backend" == x11 ]]; then
    export IBUS_ADDRESS
    bus_files=("$XDG_CONFIG_HOME"/ibus/bus/*)
    [[ ${#bus_files[@]} -eq 1 && -f "${bus_files[0]}" ]]
    export IBUS_ADDRESS_FILE="${bus_files[0]}"
    dbus-monitor --address "$IBUS_ADDRESS" "type='signal',interface='org.freedesktop.IBus.InputContext'" > "$evidence/ime-bus.log" 2>&1 & owned+=("$!")
    trace="$evidence/ime-bus.log"
    candidate='string "你好"'
    commit='member=CommitText'
else
    # GNOME can replace the IBus daemon when Xwayland starts. Resolve its current address on each query.
    unset IBUS_ADDRESS
    trace="$evidence/app.log"
    candidate='zwp_text_input_v3.*preedit_string\("你好"'
    commit='zwp_text_input_v3.*commit_string\("你好"'
fi

switch_engine() {
    local target="$1" active
    if [[ "$backend" == x11 ]]; then
        ibus engine "$target"
    elif [[ "$(ibus engine 2>/dev/null || true)" != "$target" ]]; then
        [[ "$(xdotool getactivewindow)" == "$window" ]]
        xdotool key --clearmodifiers Super+space
    fi
    for attempt in {1..30}; do
        active="$(ibus engine 2>/dev/null || true)"
        [[ "$active" == "$target" ]] && break
        sleep 0.25
    done
    [[ "$active" == "$target" ]] || { echo "IBus selected $active instead of $target" >&2; return 1; }
    echo "IBus active engine: $active"
}

probe=(/usr/bin/python3 scripts/reader-acceptance/ibus-editor.py)
"${launch[@]}" > "$evidence/app.log" 2>&1 & owned+=("$!")
if [[ "$backend" == wayland ]]; then
    for attempt in {1..60}; do
        if grep -q 'GNOME Shell started' "$evidence/mutter.log" && grep -q 'xdg_toplevel.*set_title' "$trace"; then break; fi
        sleep 0.25
    done
    xdotool windowactivate --sync "$window"
    xdotool mousemove --sync --window "$window" 345 280
    # Dismiss the initial GNOME overview so the native client can receive the seat.
    xdotool key --clearmodifiers Escape
    for attempt in {1..30}; do
        if grep -q 'wl_keyboard.*enter(' "$trace"; then break; fi
        if [[ "$attempt" == 8 ]]; then xdotool key --clearmodifiers alt+Tab; fi
        sleep 0.25
    done
    grep -m 1 'wl_keyboard.*enter(' "$trace"
fi
"${probe[@]}" directory "$evidence/workspace"
"${launch[@]}" --file "$fixture" >> "$evidence/app.log" 2>&1
"${probe[@]}" edit
switch_engine "$engine"
if [[ "$backend" == wayland ]]; then
    [[ "$(xdotool getactivewindow)" == "$window" ]]
    grep -m 1 'xdg_surface.*get_toplevel' "$trace"
fi
xdotool key --clearmodifiers ctrl+End n i h a o
for attempt in {1..30}; do
    grep -Eq "$candidate" "$trace" && break
    sleep 0.25
done
if ! grep -Eq "$candidate" "$trace" || grep -Eq "$commit" "$trace"; then
    echo 'IBus did not expose the expected uncommitted candidate' >&2
    exit 1
fi
"${probe[@]}" preedit
xdotool key --clearmodifiers space
"${probe[@]}" commit
grep -m 1 -E "$commit" "$trace"
switch_engine xkb:us::eng
"${probe[@]}" transfer
test "$before" = "$(sha256sum "$fixture")"
echo "PASS ($backend): IBus Pinyin composition, commit, workspace transfer, undo, redo, selection, and unchanged disk fixture"
"${probe[@]}" accessible-write
test "$before" = "$(sha256sum "$fixture")"
