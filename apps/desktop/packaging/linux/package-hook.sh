#!/bin/sh
# Generated maintainer entry points pass their hook name as the first argument.
set -eu
hook=$1
shift
case "$hook:$1" in
  preinst:*|prerm:*)
    if test "$hook" = preinst; then
      schema=$(dpkg-query -W -f='${X-Xdnote-Settings-Schema}' xdnote 2>/dev/null || :)
      case "$schema" in ''|1) ;; *) echo 'Installed settings schema is newer; downgrade refused.' >&2; exit 1;; esac
    fi
    # A root-owned directory prevents symlink replacement by desktop users.
    test ! -L /run/xdnote
    if test -e /run/xdnote; then
      test "$(stat -c %u:%a /run/xdnote)" = 0:755
    else
      install -d -m 0755 -o root -g root /run/xdnote
    fi
    test ! -L /run/xdnote/usage.lock
    if ! test -e /run/xdnote/usage.lock; then
      (umask 022; : > /run/xdnote/usage.lock)
    fi
    test "$(stat -c %u:%a /run/xdnote/usage.lock)" = 0:644
    # The updater holds this lock across its complete handoff. Only its matching
    # root-owned transaction may enter dpkg; unrelated manual installs refuse it.
    test ! -L /run/xdnote/update.lock
    if ! test -e /run/xdnote/update.lock; then
      (umask 077; : > /run/xdnote/update.lock)
    fi
    test "$(stat -c %u:%a /run/xdnote/update.lock)" = 0:600
    if test -e /run/xdnote/updating; then
      test ! -L /run/xdnote/updating
      test "$(stat -c %u:%a /run/xdnote/updating)" = 0:644
      test -n "${XDNOTE_UPDATE_TRANSACTION:-}"
      test "$(cat /run/xdnote/updating)" = "$XDNOTE_UPDATE_TRANSACTION"
    else
      exec 8</run/xdnote/update.lock
      if ! flock -n -x 8; then
        echo 'An xdnote update is already being prepared.' >&2
        exit 1
      fi
    fi
    exec 9</run/xdnote/usage.lock
    if ! flock -n -x 9; then
      echo 'Close xdnote before installing, updating or removing it.' >&2
      exit 1
    fi
    # Prevent new launches between dpkg hooks. Failures are cleared by dpkg's
    # abort hooks; interrupted dpkg transactions require dpkg --configure -a.
    install -m 0644 /dev/null /run/xdnote/installing
    ;;
  postinst:configure)
    update-mime-database /usr/share/mime
    update-desktop-database -q /usr/share/applications
    rm -f /run/xdnote/installing
    ;;
  postinst:abort-*|postrm:remove|postrm:purge|postrm:abort-*)
    rm -f /run/xdnote/installing
    if command -v update-mime-database >/dev/null 2>&1; then
      update-mime-database /usr/share/mime
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
      update-desktop-database -q /usr/share/applications
    fi
    ;;
esac
