"""Operate only the GNOME Shell polkit dialog in an owned, nested desktop."""
from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import time

import pyatspi


MESSAGE = 'Authentication is required to update xdnote'


def command(*args, **options):
    return subprocess.run(args, check=True, timeout=10, **options)


def windows(*selector):
    return subprocess.run(['/usr/bin/xdotool', 'search', '--onlyvisible', *selector],
                          capture_output=True, text=True, timeout=5).stdout.splitlines()


@contextmanager
def graphical_session(report):
    if not os.environ.get('DISPLAY') or not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
        raise RuntimeError('Expected the disposable graphical desktop and session bus')
    home = Path.home()
    runtime = home / 'wayland-runtime'
    runtime.mkdir(mode=0o700)
    os.environ.update(XDG_RUNTIME_DIR=str(runtime), LC_ALL='C.UTF-8', NO_AT_BRIDGE='0')
    command('/usr/bin/dbus-update-activation-environment', 'DISPLAY', 'XAUTHORITY', 'XDG_RUNTIME_DIR', 'NO_AT_BRIDGE', 'LC_ALL')
    command('/usr/bin/gsettings', 'set', 'org.gnome.desktop.interface', 'toolkit-accessibility', 'true')
    log_path = home / 'gnome-shell.log'
    with log_path.open('wb') as log:
        manager = subprocess.Popen(['/usr/bin/openbox', '--sm-disable'], stdout=log, stderr=log)
        shell = subprocess.Popen(['/usr/bin/gnome-shell', '--nested', '--wayland'], stdout=log, stderr=log,
                                 env=dict(os.environ, XDG_SESSION_TYPE='wayland', XDG_CURRENT_DESKTOP='GNOME',
                                          LIBGL_ALWAYS_SOFTWARE='1'))
        try:
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline and shell.poll() is None:
                socket = next((p.name for p in runtime.glob('wayland-*') if p.is_socket()), None)
                startup = log_path.read_bytes()
                if b'Failed to register AuthenticationAgent' in startup:
                    raise RuntimeError('GNOME Shell could not register its graphical agent with polkit')
                if socket and b'GNOME Shell started' in startup:
                    visible = windows('--pid', str(shell.pid)) or windows('--class', 'gnome-shell|mutter')
                    if visible:
                        window = visible[0]
                        command('/usr/bin/xdotool', 'windowactivate', '--sync', window)
                        version = command('/usr/bin/gnome-shell', '--version', capture_output=True, text=True).stdout.strip()
                        report['graphicalAgent'] = {'compositor': version, 'waylandSocket': socket,
                                                    'dialog': MESSAGE, 'prompts': []}
                        yield lambda *args, **kwargs: authorize(shell, window, report['graphicalAgent']['prompts'],
                                                                *args, **kwargs)
                        return
                time.sleep(.25)
            raise RuntimeError(f'GNOME Shell never opened the graphical session (exit {shell.poll()})')
        finally:
            for child in (shell, manager):
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)


def descendants(item, depth=0):
    if depth < 18:
        yield item
        for child in item:
            if child is not None:
                yield from descendants(child, depth + 1)


def wait_dialog(shell, request):
    desktop = pyatspi.Registry.getDesktop(0)
    deadline = time.monotonic() + 18
    labels = set()
    focus = set()
    while time.monotonic() < deadline and request.poll() is None and shell.poll() is None:
        desktop.clear_cache()
        labels.clear()
        focus.clear()
        for app in desktop:
            if app is None or app.get_process_id() != shell.pid:
                continue
            app.clear_cache()
            for node in descendants(app):
                try:
                    state = node.getState()
                    if not state.contains(pyatspi.STATE_SHOWING):
                        continue
                    role = node.getRoleName()
                    if role != 'password text' and node.name:
                        labels.add(node.name)
                    if state.contains(pyatspi.STATE_FOCUSED):
                        focus.add(role)
                except LookupError:
                    continue
        if {MESSAGE, 'Cancel', 'Authenticate'} <= labels and 'password text' in focus:
            return
        time.sleep(.2)
    raise RuntimeError(f'No focused GNOME polkit password dialog (request={request.poll()}, shell={shell.poll()}, '
                       f'focus={sorted(focus)}, labels={sorted(labels)[:20]})')


def authorize(shell, window, prompts, command_line, password, *, cancel=False, installed_helper=False, after_grant=None):
    request = subprocess.Popen(command_line, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    submitted = False
    name = 'cancel' if cancel else 'grant'
    try:
        try:
            wait_dialog(shell, request)
        except RuntimeError:
            if shell.poll() is None:
                command('/usr/bin/xwd', '-silent', '-id', window, '-out', str(Path.home() / 'prompt-diagnostic.xwd'))
            raise
        active = command('/usr/bin/xdotool', 'getactivewindow', capture_output=True, text=True).stdout.strip()
        if active != window:
            raise RuntimeError('The isolated GNOME authentication window lost focus')
        # Capture the owned window before any password input takes place.
        command('/usr/bin/xwd', '-silent', '-id', window, '-out', str(Path.home() / f'prompt-{name}.xwd'))
        prompts.append({'operation': name, 'actionVisible': True, 'focusedMaskedEntry': True})
        if cancel:
            command('/usr/bin/xdotool', 'key', '--clearmodifiers', 'Escape')
        else:
            command('/usr/bin/xdotool', 'type', '--clearmodifiers', '--file', '-', input=password.encode(),
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            command('/usr/bin/xdotool', 'key', '--clearmodifiers', 'Return')
            submitted = True
            if after_grant is not None:
                after_grant(request)
        return request.wait(timeout=25)
    finally:
        if request.poll() is None:
            if submitted and installed_helper:
                request.wait(timeout=170)
            else:
                request.kill()
                request.wait(timeout=5)
        request.stderr.close()
