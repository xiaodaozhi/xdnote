#!/usr/bin/env bash
# Called by orca-linux.sh inside its private Xvfb and D-Bus session.
set -euo pipefail

backend="$1"
evidence="$2"
build_dir="$3"
qt_prefix="$4"
if [[ "${XDG_RUNTIME_DIR:-}" != "$5" || ! -d "$XDG_RUNTIME_DIR" || -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    echo 'Run this check with orca-linux.sh to create the isolated session.' >&2
    exit 2
fi

probe="$(dirname -- "$0")/orca-evidence.py"
{
    echo "backend=$backend"
    echo "revision=$(git rev-parse HEAD)"
    echo "architecture=$(uname -m)"
    grep '^PRETTY_NAME=' /etc/os-release
    orca --version
    if [[ "$backend" == wayland ]]; then gnome-shell --version; fi
} | tee "$evidence/environment.txt"

owned_pids=()
finish() {
    if (( "${#owned_pids[@]}" )); then
        kill "${owned_pids[@]}" 2>/dev/null || true
        for attempt in {1..30}; do
            [[ -n "$(jobs -pr)" ]] || break
            sleep 0.1
        done
        # Orca can remain in a synchronous speech or accessibility call during shutdown.
        local running
        running="$(jobs -pr)"
        for child in "${owned_pids[@]}"; do
            if grep -qx "$child" <<< "$running"; then kill -KILL "$child" 2>/dev/null || true; fi
        done
        for child in "${owned_pids[@]}"; do wait "$child" 2>/dev/null || true; done
    fi
}
trap finish EXIT
trap 'exit 1' HUP INT TERM

openbox > "$evidence/openbox.log" 2>&1 &
owned_pids+=("$!")

if [[ "$backend" == wayland ]]; then
    gnome-shell --nested --wayland > "$evidence/mutter.log" 2>&1 &
    compositor=$!
    owned_pids+=("$compositor")
    for attempt in {1..60}; do
        kill -0 "$compositor" || { echo 'Nested GNOME Shell exited' >&2; exit 1; }
        for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
            if [[ -S "$socket" ]]; then export WAYLAND_DISPLAY="${socket##*/}"; break; fi
        done
        [[ -z "${WAYLAND_DISPLAY:-}" ]] || break
        sleep 0.5
    done
    [[ -n "${WAYLAND_DISPLAY:-}" ]] || { echo 'Nested GNOME Wayland socket did not appear' >&2; exit 1; }
    echo "GNOME Wayland socket: $WAYLAND_DISPLAY" | tee -a "$evidence/environment.txt"

    window=
    for attempt in {1..30}; do
        window="$(xdotool search --onlyvisible --pid "$compositor" 2>/dev/null | sed -n '1p' || true)"
        if [[ -z "$window" ]]; then
            window="$(xdotool search --onlyvisible --class 'gnome-shell|mutter' 2>/dev/null | sed -n '1p' || true)"
        fi
        [[ -z "$window" ]] || break
        sleep 0.5
    done
    [[ -n "$window" ]] || { echo 'Nested compositor input window did not appear' >&2; exit 1; }
fi

# Use a private audio server and a null sink. Orca still sends real speech.
export PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse.sock"
pulseaudio --daemonize=no --exit-idle-time=-1 --log-target=stderr -n \
    -L "module-native-protocol-unix socket=$XDG_RUNTIME_DIR/pulse.sock" \
    -L 'module-null-sink sink_name=xdnote_acceptance' > "$evidence/pulseaudio.log" 2>&1 &
audio_pid=$!
owned_pids+=("$audio_pid")
for attempt in {1..30}; do
    kill -0 "$audio_pid" || { echo 'Private PulseAudio exited' >&2; exit 1; }
    [[ ! -S "$XDG_RUNTIME_DIR/pulse.sock" ]] || break
    sleep 0.2
done
pactl set-default-sink xdnote_acceptance
gsettings set org.gnome.desktop.interface toolkit-accessibility true
orca --enable=speech --disable=braille --debug-file="$evidence/orca.log" > "$evidence/orca-console.log" 2>&1 &
reader_pid=$!
owned_pids+=("$reader_pid")
for attempt in {1..30}; do
    kill -0 "$reader_pid" || { echo 'Orca exited during startup' >&2; exit 1; }
    [[ ! -s "$evidence/orca.log" ]] || break
    sleep 0.2
done
[[ -s "$evidence/orca.log" ]] || { echo 'Orca did not start logging' >&2; exit 1; }
sleep 2

if [[ "${6:-}" == --quick-editor ]]; then
    export XDNOTE_QUICK_ORCA_CLIENT="$PWD/experiments/qt-document-spike/tests/orca_client.py"
    export XDNOTE_QUICK_ORCA_LOG="$evidence/orca.log" XDNOTE_ORCA_SPEECH_PROBE="$probe"
    export QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software QT_PLUGIN_PATH="$qt_prefix/plugins"
    export QML_IMPORT_PATH="$qt_prefix/qml" LD_LIBRARY_PATH="$qt_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    "$build_dir/qt_document_spike_tests" guardedOrcaAccessibility -o "$evidence/qtest.txt,txt" \
        2> >(tee "$evidence/speech-markers.txt" >&2) || { cat "$evidence/qtest.txt"; exit 1; }
    cat "$evidence/qtest.txt"
    grep -Eq '^PASS.*guardedOrcaAccessibility' "$evidence/qtest.txt"
    ! grep -Eq '^(FAIL|SKIP).*guardedOrcaAccessibility' "$evidence/qtest.txt"
    printf 'PASS: Orca on the guarded Qt Quick editor\n' | tee "$evidence/result.txt"
    exit
fi

fixture="$evidence/Reader-Check.md"
{
    cat <<'DOC'
# Native reading acceptance

Blue orchard reading sample.

[Follow the cedar heading](#cedar-anchor)

DOC
    for number in {1..40}; do printf 'Filler paragraph %s.\n\n' "$number"; done
    printf '## Cedar anchor\n\nAmber target reading sample.\n'
} > "$fixture"
(cd "$evidence" && sha256sum Reader-Check.md > fixture-before.sha256)
cp -- "$build_dir/build-identity.json" "$evidence/build-identity.json"

launch=(python scripts/desktop.py run --profile default --build-dir "$build_dir" --qt-prefix "$qt_prefix" --file "$fixture")
if [[ "$backend" == x11 ]]; then
    env QT_QPA_PLATFORM=xcb "${launch[@]}" > "$evidence/app.log" 2>&1 &
else
    # The fixture cannot connect to X11. The outer X11 window belongs to Mutter.
    env -u DISPLAY QT_QPA_PLATFORM=wayland WAYLAND_DEBUG=client "${launch[@]}" > "$evidence/app.log" 2>&1 &
fi
app_pid=$!
owned_pids+=("$app_pid")

if [[ "$backend" == x11 ]]; then
    window="$(timeout 30s xdotool search --sync --onlyvisible --name '^Reader-Check[.]md.*xdnote$' | sed -n '1p')"
    xdotool getwindowgeometry "$window" | tee "$evidence/backend.txt"
    xdotool windowactivate --sync "$window"
    xdotool mousemove --window "$window" 120 215 click 1
else
    for attempt in {1..30}; do
        if grep -q 'xdg_surface.*get_toplevel' "$evidence/app.log"; then break; fi
        kill -0 "$app_pid" || { echo 'xdnote exited before creating a Wayland window' >&2; exit 1; }
        sleep 0.5
    done
    grep -m 1 'xdg_wm_base.*get_xdg_surface' "$evidence/app.log" | tee "$evidence/backend.txt"
    grep -m 1 'xdg_surface.*get_toplevel' "$evidence/app.log" | tee -a "$evidence/backend.txt"
    xdotool windowactivate --sync "$window"
    xdotool mousemove --sync --window "$window" 345 280
    sleep 0.8
    xdotool click 1
    sleep 0.5
    xdotool click 1
fi

send_keys() {
    if [[ "$(xdotool getactivewindow)" != "$window" ]]; then
        echo 'The isolated input window lost focus' >&2
        return 1
    fi
    xdotool key --clearmodifiers "$@"
}

speech_offset() { wc -c < "$evidence/orca.log"; }
wait_for_speech() {
    python "$probe" speech "$evidence/orca.log" "$@" | tee -a "$evidence/speech-markers.txt"
}
target_state() {
    /usr/bin/python3 "$probe" window "$1" Reader-Check.md | tee -a "$evidence/atspi.txt"
}

sleep 0.7
send_keys ctrl+Home
sleep 0.5
target_state hidden

start="$(speech_offset)"
send_keys KP_Add
wait_for_speech body "$start" 'Blue orchard reading sample'
send_keys Escape
sleep 0.8

send_keys ctrl+a
sleep 0.8
start="$(speech_offset)"
send_keys Insert+Shift+Up
wait_for_speech selection "$start" 'Selected text is:'
send_keys ctrl+Home
sleep 0.5

start="$(speech_offset)"
send_keys Tab
wait_for_speech link "$start" 'Follow the cedar heading link'
send_keys Return
sleep 0.6
target_state visible

start="$(speech_offset)"
send_keys KP_Add
wait_for_speech target "$start" 'Amber target reading sample'
send_keys Escape
sleep 0.4
send_keys ctrl+w
target_state closed
for attempt in {1..30}; do
    if ! kill -0 "$app_pid" 2>/dev/null; then break; fi
    sleep 0.2
done
if kill -0 "$app_pid" 2>/dev/null; then
    echo 'xdnote did not exit after the owned fixture closed' >&2
    exit 1
fi
wait "$app_pid"
(cd "$evidence" && sha256sum Reader-Check.md > fixture-after.sha256 && cmp fixture-before.sha256 fixture-after.sha256)
printf 'PASS: Orca on %s, fixture SHA-256 unchanged\n' "$backend" | tee "$evidence/result.txt"
