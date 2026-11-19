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


def action(node):
    commands = node.queryAction()
    for index in range(commands.nActions):
        if commands.getName(index) in ('click', 'activate') and commands.doAction(index):
            return
    raise ValueError(f'No activation action for {node.name}')


def select_child(node):
    while node.parent:
        try:
            if node.parent.querySelection().selectChild(node.getIndexInParent()):
                return
        except NotImplementedError:
            pass
        node = node.parent
    raise ValueError('No accessible selection for the requested Nautilus item')


def wayland_window_pid(fixture, binary, socket):
    deadline = time.monotonic() + 15
    desktop = pyatspi.Registry.getDesktop(0)
    while time.monotonic() < deadline:
        desktop.clear_cache()
        for app in desktop:
            if app is None or (app.name or '').casefold() != 'xdnote':
                continue
            app.clear_cache()
            if not any(node is not None and (node.name or '').startswith(fixture.name) and showing(node)
                       for node in app):
                continue
            pid = app.get_process_id()
            if Path(f'/proc/{pid}/exe').resolve(strict=True) != binary:
                raise ValueError('The opened Wayland window is not owned by the installed product')
            environment = set(Path(f'/proc/{pid}/environ').read_bytes().split(b'\0'))
            if b'QT_QPA_PLATFORM=wayland' not in environment or f'WAYLAND_DISPLAY={socket}'.encode() not in environment:
                raise ValueError('The installed product did not inherit the native Wayland session')
            return pid
        time.sleep(0.2)
    raise ValueError(f'No installed Wayland document window for {fixture.name}')


def start_wayland(log):
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    compositor = subprocess.Popen(['/usr/bin/gnome-shell', '--nested', '--wayland'], stdout=log, stderr=log,
                                  env=dict(os.environ, XDG_SESSION_TYPE='wayland', LIBGL_ALWAYS_SOFTWARE='1'))
    deadline = time.monotonic() + 15
    try:
        while time.monotonic() < deadline and compositor.poll() is None:
            socket = next((item.name for item in runtime.glob('wayland-*') if item.is_socket()), None)
            if socket:
                windows = subprocess.run(['/usr/bin/xdotool', 'search', '--onlyvisible', '--class', 'gnome-shell|mutter'],
                                         capture_output=True, text=True, timeout=3).stdout.splitlines()
                if windows:
                    return compositor, socket, windows[0]
            time.sleep(0.2)
        raise ValueError('Nested GNOME Wayland desktop did not become ready')
    except BaseException:
        if compositor.poll() is None:
            compositor.terminate()
            compositor.wait(timeout=5)
        raise


def verify(backend, binary, evidence, samples):
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

    result = {'ok': False, 'backend': backend, 'nautilus': execute('/usr/bin/nautilus', '--version'),
              'initialDefault': default_handler()}
    wayland = backend == 'wayland'
    product_pids = set()
    with (evidence / 'nautilus-session.log').open('w', encoding='utf-8') as log:
        manager = subprocess.Popen(['/usr/bin/openbox', '--sm-disable'], stdout=log, stderr=log)
        compositor = files = None
        client = dict(os.environ, GDK_BACKEND='x11', QT_QPA_PLATFORM='xcb')
        window = None
        socket = None
        if wayland:
            compositor, socket, window = start_wayland(log)
            client.update(GDK_BACKEND='wayland', QT_QPA_PLATFORM='wayland', WAYLAND_DISPLAY=socket,
                          WAYLAND_DEBUG='client', XDG_SESSION_TYPE='wayland')
            del client['DISPLAY']
            result.update(compositor=execute('/usr/bin/gnome-shell', '--version'), waylandSocket=socket)
            execute('/usr/bin/xdotool', 'windowactivate', '--sync', window)
        files = subprocess.Popen(['/usr/bin/nautilus', '--new-window', samples[0].parent],
                                 env=client, stdout=log, stderr=log)

        def key(*keys):
            execute('/usr/bin/xdotool', 'key', '--clearmodifiers', *keys)

        def sample_node(sample):
            return lookup(sample.name, [sample.name])

        def bring_forward():
            if not wayland:
                execute('/usr/bin/xdotool', 'windowactivate', '--sync', window)
            elif not any(node.getRoleName() == 'frame' and node.getState().contains(pyatspi.STATE_ACTIVE)
                         for node in nautilus_nodes()):
                key('alt+Tab')
                time.sleep(0.6)

        def select_sample(sample):
            if not wayland:
                click(sample_node(sample))
                return
            key('ctrl+s')
            lookup('pattern selector', ['Select Items Matching'], ['dialog'])
            execute('/usr/bin/xdotool', 'type', '--clearmodifiers', sample.name.split('-', 1)[0] + '-*')
            key('Return')
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if sample_node(sample).getState().contains(pyatspi.STATE_SELECTED):
                    return
                time.sleep(0.15)
            raise ValueError(f'Nautilus did not select {sample.name}')

        def opened(sample):
            return wayland_window_pid(sample, binary, socket) if wayland else window_pid(sample, binary)

        activate = action if wayland else click
        choose = select_child if wayland else click
        try:
            once, warm, cold = samples
            item = sample_node(once)
            if wayland:
                select_sample(once)
                key('shift+F10')
            else:
                windows = execute('/usr/bin/xdotool', 'search', '--sync', '--onlyvisible', '--all',
                                  '--pid', str(files.pid), '--name', re.escape(samples[0].parent.name))
                window = windows.splitlines()[0]
                click(item, button='3')
            activate(lookup('Open With Other Application', ['Open With Other Application']))
            choose(lookup('xdnote in Open With', ['xdnote']))
            activate(lookup('Open With confirmation', ['Select', 'Open'], ['push button']))
            initial_pid = opened(once)
            product_pids.add(initial_pid)
            if default_handler() != FALLBACK:
                raise ValueError('Opening with xdnote once changed the default')
            result['openOnce'] = once.name

            bring_forward()
            select_sample(warm)
            key('alt+Return')
            choose(lookup('Open With tab', ['Open With'], ['page tab']))
            choose(lookup('xdnote in Properties', ['xdnote']))
            activate(lookup('Set as default', ['Set as default'], ['push button']))
            if default_handler() != DESKTOP:
                raise ValueError('The Nautilus button did not set xdnote as the default')
            result['selectedDefault'] = default_handler()
            key('alt+F4')

            bring_forward()
            if wayland:
                select_sample(warm)
                key('Return')
            else:
                click(sample_node(warm), count='2')
            warm_pid = opened(warm)
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
            bring_forward()
            if wayland:
                select_sample(cold)
                key('Return')
            else:
                click(sample_node(cold), count='2')
            cold_pid = opened(cold)
            product_pids.add(cold_pid)
            if cold_pid == initial_pid:
                raise ValueError('Nautilus default opening did not cold launch the installed app')
            if wayland:
                trace = (evidence / 'nautilus-session.log').read_text(encoding='utf-8')
                if not re.search(r'xdg_wm_base.*get_xdg_surface', trace) or not re.search(
                        r'xdg_toplevel.*set_title\("' + re.escape(cold.name) + r'[^"\n]*xdnote', trace):
                    raise ValueError('Wayland shell protocol did not show the installed product window')
                result['nativeWayland'] = True
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
            for process in (files, compositor, manager):
                if process is not None and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == '__main__':
    backend = sys.argv[1]
    if backend not in ('x11', 'wayland'):
        raise SystemExit('Expected x11 or wayland')
    product, result_directory, *fixtures = map(Path, sys.argv[2:])
    if len(fixtures) != 3:
        raise SystemExit('Expected the launcher, evidence directory and three owned fixtures')
    verify(backend, product, result_directory, fixtures)
