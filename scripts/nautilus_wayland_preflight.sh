#!/usr/bin/env bash
set -euo pipefail
[[ "${RUNNER_ENVIRONMENT:-}" == github-hosted ]]
export HOME="$PWD/build/preflight/private-home"
export XDG_CONFIG_HOME="$HOME/config" XDG_CACHE_HOME="$HOME/cache" XDG_DATA_HOME="$HOME/data"
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export XDG_RUNTIME_DIR="$(mktemp -d)"
trap 'rmdir "$XDG_RUNTIME_DIR" 2>/dev/null || true' EXIT
export GDK_BACKEND=x11 GSETTINGS_BACKEND=memory NO_AT_BRIDGE=0 GTK_MODULES=atk-bridge LC_ALL=C.UTF-8
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME LIBGL_ALWAYS_SOFTWARE=1
samples="$HOME/File manager acceptance"
mkdir -p "$XDG_DATA_HOME/applications" "$samples" build/preflight/evidence
cat > "$XDG_DATA_HOME/applications/xdnote.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=xdnote
Exec=/usr/bin/mousepad %f
MimeType=text/markdown;
DESKTOP
for item in once warm cold; do
  printf '# Owned Wayland stock editor preflight\n' > "$samples/$item.md"
done
xvfb-run -a -s '-screen 0 1440x1080x24 +extension GLX' dbus-run-session -- \
  /usr/bin/python3 scripts/check_nautilus_open.py wayland /usr/bin/mousepad "$PWD/build/preflight/evidence" \
  "$samples/once.md" "$samples/warm.md" "$samples/cold.md"
