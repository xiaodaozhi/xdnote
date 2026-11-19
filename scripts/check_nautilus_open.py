"""Drive the installed Nautilus UI inside the package test's private desktop."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from gi.repository import Gio
import pyatspi

from check_linux_package_install import window_pid


MIME = 'text/markdown'
FALLBACK = 'xdnote-acceptance-fallback.desktop'
DESKTOP = 'xdnote.desktop'


def execute(*args):
    return subprocess.check_output(args, text=True, timeout=8).strip()


def showing(node):
    return node.getState().contains(pyatspi.STATE_SHOWING)


def descendants(node, depth=0):
    if depth < 18:
        yield node
        for child in node:
            if child is not None:
                yield from descendants(child, depth + 1)


def nautilus_nodes():
    desktop = pyatspi.Registry.getDesktop(0)
    for app in desktop:
        if app.name.casefold() in ('nautilus', 'org.gnome.nautilus', 'files'):
            yield from descendants(app)


def lookup(label, names, roles=()):
    deadline = time.monotonic() + 10
    wanted = {name.casefold() for name in names}
    while time.monotonic() < deadline:
        for node in nautilus_nodes():
            try:
                if (node.name.rstrip('… .').casefold() in wanted and showing(node)
                        and (not roles or node.getRoleName() in roles)):
                    return node
            except LookupError:
                continue
        time.sleep(0.15)
    raise ValueError(f'No visible Nautilus control for {label}')


def click(node, button='1', count='1'):
    bounds = node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
    if bounds.width < 1 or bounds.height < 1:
        raise ValueError(f'No clickable bounds for {node.name}')
    execute('/usr/bin/xdotool', 'mousemove', str(bounds.x + bounds.width // 2),
            str(bounds.y + bounds.height // 2), 'click', '--repeat', count, button)


def default_handler():
    handler = Gio.AppInfo.get_default_for_type(MIME, False)
    return handler.get_id() if handler is not None else None


def wayland_socket(compositor):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline and compositor.poll() is None:
        for socket in Path(os.environ['XDG_RUNTIME_DIR']).glob('wayland-*'):
            if socket.is_socket():
                return socket.name
        time.sleep(0.2)
    raise ValueError('The private GNOME Wayland socket did not appear')


def wayland_document(sample, binary):
    deadline = time.monotonic() + 15
    desktop = pyatspi.Registry.getDesktop(0)
    while time.monotonic() < deadline:
        desktop.clear_cache()
        for app in desktop:
            if app is None or (app.name or '').casefold() != 'xdnote':
                continue
            app.clear_cache()
            for window in app:
                if window is not None and showing(window) and sample.name in (window.name or ''):
                    pid = app.get_process_id()
                    if Path(f'/proc/{pid}/exe').resolve(strict=True) != binary:
                        raise ValueError('The document window belongs to a different executable')
                    maps = Path(f'/proc/{pid}/maps').read_text()
                    if not re.search(r'/platforms/libqwayland[^/\n ]*\.so', maps):
                        raise ValueError('The installed application did not load a Wayland platform plugin')
                    return pid
        time.sleep(0.2)
    raise ValueError(f'No installed Wayland document window for {sample.name}')


def activate(node):
    try:
        action = node.queryAction()
    except NotImplementedError:
        select_item(node)
        return
    for index in range(action.nActions):
        name = action.getName(index).casefold()
        if name in ('click', 'activate', 'press', 'switch'):
            if not action.doAction(index):
                raise ValueError(f'Nautilus rejected {name} on {node.name}')
            return
    select_item(node)


def select_item(node, *, clear=False):
    selection = node.parent.querySelection()
    if clear:
        selection.clearSelection()
    if not selection.selectChild(node.getIndexInParent()):
        raise ValueError(f'Nautilus did not select {node.name}')


def open_item(node):
    action = node.queryAction()
    for index in range(action.nActions):
        if action.getName(index).casefold() == 'open' and action.doAction(index):
            return
    raise ValueError(f'Nautilus did not open {node.name}')


def verify(binary, evidence, samples, backend):
    if (os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted'
            or not os.environ.get('DBUS_SESSION_BUS_ADDRESS') or not os.environ.get('DISPLAY')):
        raise ValueError('Use the disposable package test session')
    if any(sample.parent != samples[0].parent for sample in samples):
        raise ValueError('Expected owned samples in one private folder')
    fallback = Path(os.environ['XDG_DATA_HOME']) / 'applications' / FALLBACK
    fallback.parent.mkdir(parents=True)
    fallback.write_text('[Desktop Entry]\nType=Application\nName=Acceptance fallback\n'
                        'Exec=/usr/bin/false %f\nMimeType=text/markdown;\n', encoding='utf-8')
    execute('/usr/bin/gio', 'mime', MIME, FALLBACK)
    if default_handler() != FALLBACK:
        raise ValueError('Could not seed the private initial Markdown default')

    result = {'ok': False, 'nautilus': execute('/usr/bin/nautilus', '--version'), 'backend': backend,
              'initialDefault': default_handler()}
    product_pids = set()
    with (evidence / 'nautilus-session.log').open('w', encoding='utf-8') as log:
        manager = subprocess.Popen(['/usr/bin/openbox', '--sm-disable'], stdout=log, stderr=log)
        session_processes = [manager]
        wayland = backend == 'wayland'
        press = activate if wayland else click
        opened_document = wayland_document if wayland else window_pid

        def bring_forward(window):
            execute('/usr/bin/xdotool', 'windowactivate', '--sync', window)
            if wayland and not folder_active():
                execute('/usr/bin/xdotool', 'key', '--clearmodifiers', 'alt+Tab')
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    if folder_active():
                        return
                    time.sleep(0.15)
                raise ValueError('The owned Wayland Nautilus window did not receive focus')

        def sample_node(sample):
            return lookup(sample.name, [sample.name])

        def folder_active():
            folder = lookup('the owned Nautilus window', [samples[0].parent.name], ['frame'])
            folder.clear_cache()
            return folder.getState().contains(pyatspi.STATE_ACTIVE)

        def open_sample(sample):
            if wayland:
                open_item(sample_node(sample))
            else:
                click(sample_node(sample), count='2')

        try:
            once, warm, cold = samples
            files_env = os.environ.copy()
            if wayland:
                compositor = subprocess.Popen(['/usr/bin/gnome-shell', '--nested', '--wayland'], stdout=log, stderr=log)
                session_processes.append(compositor)
                socket = wayland_socket(compositor)
                result.update(compositor=execute('/usr/bin/gnome-shell', '--version'), waylandSocket=socket)
                files_env.pop('DISPLAY')
                files_env.update(WAYLAND_DISPLAY=socket, GDK_BACKEND='wayland', QT_QPA_PLATFORM='wayland',
                                 WAYLAND_DEBUG='client')
            files = subprocess.Popen(['/usr/bin/nautilus', '--new-window', samples[0].parent],
                                     env=files_env, stdout=log, stderr=log)
            session_processes.append(files)
            item = sample_node(once)
            match = ('--pid', str(compositor.pid)) if wayland else (
                '--pid', str(files.pid), '--name', re.escape(samples[0].parent.name))
            windows = execute('/usr/bin/xdotool', 'search', '--sync', '--onlyvisible', '--all',
                              *match)
            window = windows.splitlines()[0]
            if wayland:
                bring_forward(window)
                select_item(item, clear=True)
                item.parent.queryComponent().grabFocus()
                execute('/usr/bin/xdotool', 'key', '--clearmodifiers', 'Shift+F10')
            else:
                click(item, button='3')
            press(lookup('Open With Other Application', ['Open With Other Application']))
            choose = select_item if wayland else click
            choose(lookup('xdnote in Open With', ['xdnote']))
            press(lookup('Open With confirmation', ['Select', 'Open'], ['push button']))
            initial_pid = opened_document(once, binary)
            product_pids.add(initial_pid)
            if default_handler() != FALLBACK:
                raise ValueError('Opening with xdnote once changed the default')
            result['openOnce'] = once.name

            bring_forward(window)
            if wayland:
                item = sample_node(warm)
                select_item(item, clear=True)
                item.parent.queryComponent().grabFocus()
            else:
                click(sample_node(warm))
            execute('/usr/bin/xdotool', 'key', 'alt+Return')
            press(lookup('Open With tab', ['Open With'], ['page tab']))
            choose(lookup('xdnote in Properties', ['xdnote']))
            press(lookup('Set as default', ['Set as default'], ['push button']))
            deadline = time.monotonic() + 5
            while default_handler() != DESKTOP and time.monotonic() < deadline:
                time.sleep(0.15)
            if default_handler() != DESKTOP:
                raise ValueError('The Nautilus button did not set xdnote as the default')
            result['selectedDefault'] = default_handler()
            execute('/usr/bin/xdotool', 'key', 'alt+F4')

            bring_forward(window)
            open_sample(warm)
            warm_pid = opened_document(warm, binary)
            product_pids.add(warm_pid)
            if warm_pid != initial_pid:
                raise ValueError('Nautilus default opening did not forward to the running instance')
            result['defaultWarm'] = warm.name
            result['sameWarmProcess'] = True

            os.kill(initial_pid, signal.SIGTERM)
            deadline = time.monotonic() + 8
            while Path(f'/proc/{initial_pid}/exe').exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            if Path(f'/proc/{initial_pid}/exe').exists():
                raise ValueError('The owned test instance did not exit')
            product_pids.remove(initial_pid)
            bring_forward(window)
            open_sample(cold)
            cold_pid = opened_document(cold, binary)
            product_pids.add(cold_pid)
            if cold_pid == initial_pid:
                raise ValueError('Nautilus default opening did not cold launch the installed app')
            result.update(defaultCold=cold.name, ok=True)
        except Exception:
            for node in nautilus_nodes():
                try:
                    if node.name and showing(node):
                        print(node.getRoleName(), node.name, file=log)
                except LookupError:
                    continue
            raise
        finally:
            (evidence / 'nautilus.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
            for pid in product_pids:
                if Path(f'/proc/{pid}/exe').resolve() == binary:
                    os.kill(pid, signal.SIGTERM)
            for process in reversed(session_processes):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == '__main__':
    backend, *paths = sys.argv[1:]
    product, result_directory, *fixtures = map(Path, paths)
    if backend not in ('x11', 'wayland') or len(fixtures) != 3:
        raise SystemExit('Expected a backend, launcher, evidence directory and three owned fixtures')
    verify(product, result_directory, fixtures, backend)
