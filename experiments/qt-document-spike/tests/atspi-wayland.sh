#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "$0")/../../.." && pwd -P)"
cd -- "$root"
evidence="$root/build/qt-document-spike/wayland-atspi"

if [[ "${1:-}" != "--session" ]]; then
    [[ "$(uname -s)" == Linux ]] || { echo 'Linux is required.' >&2; exit 2; }
    for tool in xvfb-run xauth dbus-run-session openbox gnome-shell timeout; do
        command -v "$tool" >/dev/null || { echo "Missing command: $tool" >&2; exit 2; }
    done
    /usr/bin/python3 -c 'import pyatspi; from gi.repository import Atspi'
    mkdir -p -- "$evidence"
    runtime="$(mktemp -d "$root/build/atspi-wayland.XXXXXX")"
    trap 'rm -rf -- "$runtime"' EXIT
    export XDG_RUNTIME_DIR="$runtime"
    export XDG_CONFIG_HOME="$evidence/config" XDG_CACHE_HOME="$evidence/cache" XDG_DATA_HOME="$evidence/data"
    install -d -m 700 -- "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
    unset DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS WAYLAND_DISPLAY
    export LC_ALL=C.UTF-8 QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 NO_AT_BRIDGE=0
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME LIBGL_ALWAYS_SOFTWARE=1
    timeout --kill-after=15s 180s xvfb-run -a -s '-screen 0 1440x1080x24 +extension GLX' \
        dbus-run-session -- bash "$0" --session 2>&1 | tee "$evidence/run.log"
    exit
fi

[[ -n "${DISPLAY:-}" && -n "${DBUS_SESSION_BUS_ADDRESS:-}" && -d "${XDG_RUNTIME_DIR:-}" ]] || {
    echo 'A private X11, D-Bus and runtime session is required.' >&2
    exit 2
}
owned=()
finish() {
    if (( ${#owned[@]} )); then
        kill "${owned[@]}" 2>/dev/null || true
        for child in "${owned[@]}"; do wait "$child" 2>/dev/null || true; done
    fi
}
trap finish EXIT
trap 'exit 1' HUP INT TERM

openbox --sm-disable > "$evidence/openbox.log" 2>&1 & owned+=("$!")
gnome-shell --nested --wayland > "$evidence/gnome-shell.log" 2>&1 &
compositor=$!
owned+=("$compositor")
unset WAYLAND_DISPLAY
for attempt in {1..60}; do
    kill -0 "$compositor" || { echo 'Nested GNOME Shell exited.' >&2; exit 1; }
    for socket in "$XDG_RUNTIME_DIR"/wayland-*; do
        if [[ -S "$socket" ]]; then export WAYLAND_DISPLAY="${socket##*/}"; break; fi
    done
    [[ -z "${WAYLAND_DISPLAY:-}" ]] || break
    sleep 0.5
done
[[ -n "${WAYLAND_DISPLAY:-}" ]] || { echo 'The private Wayland socket did not appear.' >&2; exit 1; }
{
    echo "revision=$(git rev-parse HEAD)"
    echo "architecture=$(uname -m)"
    grep '^PRETTY_NAME=' /etc/os-release
    gnome-shell --version
} > "$evidence/environment.txt"

export XDNOTE_ATSPI_CLIENT="$root/experiments/qt-document-spike/tests/atspi_client.py"
env -u DISPLAY QT_QPA_PLATFORM=wayland QT_QUICK_BACKEND=software WAYLAND_DEBUG=client \
    "$root/build/qt-document-spike/qt_document_spike_tests" guardedAtspiAccessibility \
    -o "$evidence/qtest.txt,txt" > "$evidence/wayland.log" 2>&1
cat "$evidence/qtest.txt"
grep -Eq 'PASS.*guardedAtspiAccessibility' "$evidence/qtest.txt"
! grep -Eq '(SKIP|FAIL).*guardedAtspiAccessibility' "$evidence/qtest.txt"
grep -m 1 'get_xdg_surface' "$evidence/wayland.log"
grep -m 1 'get_toplevel' "$evidence/wayland.log"
echo 'PASS: separate AT-SPI client on the private GNOME Wayland session.'
