#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd -P)"
cd -- "$script_dir/../.."

if [[ ( $# -ne 3 && $# -ne 4 ) || ( "$1" != x11 && "$1" != wayland ) ||
      ( $# -eq 4 && ( "$4" != --quick-editor || "$1" != x11 ) ) ]]; then
    echo "Usage: bash scripts/reader-acceptance/orca-linux.sh {x11|wayland} BUILD_DIR QT_PREFIX [--quick-editor (x11)]" >&2
    exit 2
fi
if [[ "$(uname -s)" != Linux ]]; then
    echo "Orca acceptance needs a Linux host." >&2
    exit 2
fi

backend="$1"
build_dir="$2"
qt_prefix="$3"
mode="${4:-}"
tools=(xvfb-run xauth dbus-run-session openbox orca pulseaudio pactl gsettings xdotool timeout python)
if [[ "$backend" == wayland ]]; then tools+=(gnome-shell); fi
for tool in "${tools[@]}"; do
    command -v "$tool" >/dev/null || { echo "Missing command: $tool" >&2; exit 2; }
done
/usr/bin/python3 -c 'import pyatspi; from gi.repository import Atspi' || {
    echo "Install the system python3-pyatspi and AT-SPI bindings." >&2
    exit 2
}

output="$PWD/build/reader-acceptance/orca-$backend"
if [[ "$mode" == --quick-editor ]]; then output="$PWD/build/reader-acceptance/orca-quick-editor"; fi
mkdir -p -- "$output"
evidence="$(mktemp -d "$output/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
export XDG_CONFIG_HOME="$evidence/config"
export XDG_CACHE_HOME="$evidence/cache"
export XDG_DATA_HOME="$evidence/data"
# Keep Unix socket paths short even when the evidence directory is deeply nested.
runtime="$(mktemp -d "$PWD/build/orca-runtime.XXXXXX")"
trap 'rm -rf -- "$runtime"' EXIT
export XDG_RUNTIME_DIR="$runtime"
install -d -m 700 -- "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
unset DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS WAYLAND_DISPLAY PULSE_SERVER PULSE_COOKIE SPEECHD_ADDRESS
export LC_ALL=C.UTF-8 QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 NO_AT_BRIDGE=0
export XDG_SESSION_TYPE="$backend"
if [[ "$backend" == wayland ]]; then
    export XDG_CURRENT_DESKTOP=GNOME LIBGL_ALWAYS_SOFTWARE=1
else
    export XDG_CURRENT_DESKTOP=Openbox
fi

echo "Evidence directory: $evidence"
timeout --kill-after=15s 240s xvfb-run -a -s '-screen 0 1440x1080x24 +extension GLX' \
    dbus-run-session -- bash "$script_dir/orca-session.sh" "$backend" "$evidence" "$build_dir" "$qt_prefix" "$runtime" "$mode" \
    2>&1 | tee "$evidence/run.log"
