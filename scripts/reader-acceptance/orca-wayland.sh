#!/usr/bin/env bash
set -euo pipefail
evidence="$PWD/build/evidence/reader-wayland"
mkdir -p "$evidence/home"
export XDG_CONFIG_HOME="$evidence/home/config"
export XDG_CACHE_HOME="$evidence/home/cache"
export XDG_DATA_HOME="$evidence/home/data"
export XDG_RUNTIME_DIR="$evidence/runtime"
install -d -m 700 "$XDG_RUNTIME_DIR"
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME
export LC_ALL=C.UTF-8 LIBGL_ALWAYS_SOFTWARE=1
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 NO_AT_BRIDGE=0

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

cat /etc/os-release
gnome-shell --version
orca --version
openbox > "$evidence/outer-openbox.log" 2>&1 &
owned_pids+=("$!")
gnome-shell --nested --wayland > "$evidence/mutter.log" 2>&1 &
shell_pid=$!
owned_pids+=("$shell_pid")
WAYLAND_DISPLAY=
for attempt in {1..60}; do
    kill -0 "$shell_pid" || { cat "$evidence/mutter.log"; exit 1; }
    for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
        if [ -S "$socket" ]; then WAYLAND_DISPLAY="${socket##*/}"; break; fi
    done
    [ -z "$WAYLAND_DISPLAY" ] || break
    sleep 0.5
done
test -n "$WAYLAND_DISPLAY"
export WAYLAND_DISPLAY
echo "GNOME nested compositor published $WAYLAND_DISPLAY"
window=
for attempt in {1..30}; do
    window="$(xdotool search --onlyvisible --pid "$shell_pid" 2>/dev/null | sed -n '1p' || true)"
    if [ -z "$window" ]; then
        window="$(xdotool search --onlyvisible --class 'gnome-shell|mutter' 2>/dev/null | sed -n '1p' || true)"
    fi
    [ -z "$window" ] || break
    sleep 0.5
done
if [ -z "$window" ]; then xwininfo -root -tree; exit 1; fi
echo "Outer input window $window"
pulseaudio --start --exit-idle-time=-1
pactl load-module module-null-sink sink_name=xdnote_wayland
pactl set-default-sink xdnote_wayland
gsettings set org.gnome.desktop.interface toolkit-accessibility true
orca --enable=speech --disable=braille --debug-file="$evidence/orca.log" > "$evidence/orca-console.log" 2>&1 &
owned_pids+=("$!")
screen_reader=${owned_pids[-1]}
sleep 2
kill -0 "$screen_reader"
if [ "${XDNOTE_WAYLAND_SETUP_ONLY:-0}" = "1" ]; then exit 0; fi

# The fixture client has only the GNOME Wayland connection. X11 is used
# exclusively for the nested compositor's output and host input.
env -u DISPLAY QT_QPA_PLATFORM=wayland WAYLAND_DEBUG=client \
    python scripts/desktop.py run --profile default --build-dir build/desktop-admission \
    --qt-prefix build/Qt/6.8.2/gcc_64 --file "$fixture" > "$evidence/app.log" 2>&1 &
owned_pids+=("$!")
for attempt in {1..30}; do
    if grep -Eq 'xdg_wm_base.*get_xdg_surface' "$evidence/app.log"; then break; fi
    sleep 0.5
done
grep -Em 2 'xdg_wm_base.*get_xdg_surface|xdg_surface.*get_toplevel' "$evidence/app.log"
xdotool windowactivate --sync "$window"
xdotool mousemove --sync --window "$window" 345 280
sleep 0.8
xdotool click 1
sleep 0.5
xdotool click 1
sleep 0.7
xdotool key --clearmodifiers ctrl+Home
sleep 0.5
/usr/bin/python3 scripts/reader-acceptance/orca-target.py hidden

speech_start() { wc -c < "$evidence/orca.log"; }
wait_speech() {
    python scripts/reader-acceptance/wait-orca-speech.py "$evidence/orca.log" "$@"
}
start="$(speech_start)"
xdotool key --clearmodifiers KP_Add
wait_speech "$start" "Blue orchard reading sample"
xdotool key --clearmodifiers Escape
sleep 0.8
xdotool key --clearmodifiers ctrl+a
sleep 0.8
start="$(speech_start)"
xdotool key --clearmodifiers Insert+Shift+Up
wait_speech "$start" "Selected text is:"
xdotool key --clearmodifiers ctrl+Home
sleep 0.5
start="$(speech_start)"
xdotool key --clearmodifiers Tab
wait_speech "$start" "Follow the cedar heading link"
start="$(speech_start)"
xdotool key --clearmodifiers Return
sleep 0.6
/usr/bin/python3 scripts/reader-acceptance/orca-target.py visible
xdotool key --clearmodifiers KP_Add
wait_speech "$start" "Amber target reading sample"
xdotool key --clearmodifiers Escape
sleep 0.4
xdotool key --clearmodifiers ctrl+w
/usr/bin/python3 scripts/reader-acceptance/orca-target.py closed
test "$original" = "$(sha256sum "$fixture")"
echo "GNOME Wayland Orca acceptance passed; fixture bytes unchanged"
