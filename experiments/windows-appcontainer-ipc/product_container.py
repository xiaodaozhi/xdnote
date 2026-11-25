"""Test the product SingleInstance implementation in disposable AppContainers."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import uuid

import cross_container as native

native.a.ConvertSidToStringSidW.argtypes = [wintypes.LPVOID, ctypes.POINTER(wintypes.LPWSTR)]
native.k.LocalFree.argtypes = [wintypes.LPVOID]
native.u.GetAppContainerFolderPath.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.LPWSTR)]
native.u.GetAppContainerFolderPath.restype = ctypes.c_long
ole = ctypes.WinDLL('ole32')
ole.CoTaskMemFree.argtypes = [wintypes.LPVOID]

def main():
    root = Path.cwd().resolve()
    base = (root / 'build/cycle-268-appcontainer').resolve()
    if not base.is_relative_to(root / 'build'):
        raise RuntimeError('Unexpected experiment output location')
    binaries = base / 'out'
    runtime = base / ('product-isolation-' + uuid.uuid4().hex)
    runtime.mkdir()
    profiles = []
    children = []
    try:
        for filename in ('product_instance_probe.exe', 'container_isolation_probe.exe'):
            shutil.copy2(binaries / filename, runtime / filename)
        for filename in ('Qt6Core.dll', 'Qt6Network.dll'):
            shutil.copy2(root / 'build/Qt/6.8.2/msvc2022_64/bin' / filename, runtime / filename)
        for filename in ('msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            shutil.copy2(root / 'build/toolchain/windows-runtime/runtime' / filename, runtime / filename)
        for _ in range(2):
            name = 'xdnote.product.test.' + uuid.uuid4().hex
            sid = wintypes.LPVOID()
            result = native.u.CreateAppContainerProfile(name, 'xdnote temporary IPC test', 'Disposable IPC test', None, 0, ctypes.byref(sid))
            if result:
                raise OSError(result, 'CreateAppContainerProfile')
            profiles.append((name, sid))
        if native.a.EqualSid(profiles[0][1], profiles[1][1]):
            raise RuntimeError('Temporary container identities are not distinct')
        scope = None
        for index, (_, sid) in enumerate(profiles):
            sid_text = wintypes.LPWSTR()
            if not native.a.ConvertSidToStringSidW(sid, ctypes.byref(sid_text)):
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                for path in runtime.iterdir():
                    grant = subprocess.run(['icacls', str(path), '/grant', '*' + sid_text.value + ':(RX)'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if grant.returncode:
                        raise RuntimeError('Could not grant access to a temporary test binary')
                if index == 0:
                    folder = wintypes.LPWSTR()
                    status = native.u.GetAppContainerFolderPath(sid_text, ctypes.byref(folder))
                    if status:
                        raise OSError(status, 'GetAppContainerFolderPath')
                    try:
                        scope = (Path(folder.value) / ('xdnote-qa-' + uuid.uuid4().hex)).as_posix()
                    finally:
                        ole.CoTaskMemFree(ctypes.cast(folder, wintypes.LPVOID))
            finally:
                native.k.LocalFree(ctypes.cast(sid_text, wintypes.LPVOID))

        first, other = profiles[0][1], profiles[1][1]
        suffix = 'xdnote-v1-' + hashlib.sha256(scope.encode('utf-8')).hexdigest()[:24]
        pipe = '\\\\.\\pipe\\LOCAL\\' + suffix
        application = runtime / 'product_instance_probe.exe'
        raw_probe = runtime / 'container_isolation_probe.exe'

        def spawn(program, sid, mode, argument):
            child = native.Child(program, sid, mode, argument, io=True)
            children.append(child)
            lines = queue.Queue()
            def read():
                while True:
                    value = child.stdout.readline()
                    lines.put(value)
                    if not value:
                        break
            threading.Thread(target=read, daemon=True).start()
            return child, lines

        def next_line(lines):
            try:
                return lines.get(timeout=10).decode('utf-8', 'replace').strip()
            except queue.Empty:
                raise RuntimeError('Timed out waiting for isolated process')

        def start(program, sid, mode, argument):
            child, lines = spawn(program, sid, mode, argument)
            return child, lines, next_line(lines)

        def client(program, sid, mode, argument):
            child = native.Child(program, sid, mode, argument)
            children.append(child)
            try:
                return child.wait()
            finally:
                child.close()

        owner, _, ready = start(raw_probe, first, 'server-qt', pipe)
        if ready != 'ready':
            raise RuntimeError('Could not establish the pre-existing test endpoint')
        blocked, _, state = start(application, first, 'server', scope)
        preclaimed = {'state': state, 'exit': blocked.wait()}
        blocked.close()
        still_available = client(raw_probe, first, 'open', pipe) == 0
        owner.stdin.write(b'x')
        owner_exit = owner.wait()
        owner.close()

        server, lines, state = start(application, first, 'server', scope)
        if state != 'ready':
            raise RuntimeError('Product listener failed to start: ' + state)
        other_open = client(raw_probe, other, 'open', pipe)
        sequential = [client(application, first, 'client', scope) for _ in range(2)]
        parallel = []
        for _ in range(4):
            child = native.Child(application, first, 'client', scope)
            children.append(child)
            parallel.append(child)
        concurrent = [child.wait() for child in parallel]
        for child in parallel:
            child.close()
        received = []
        for _ in range(6):
            try:
                received.append(lines.get(timeout=5).decode('utf-8', 'replace').strip())
            except queue.Empty:
                received.append('timeout')
        server.stdin.write(b'x')
        server_exit = server.wait()
        server.close()
        race_scope = (Path(scope).parent / ('xdnote-race-' + uuid.uuid4().hex)).as_posix()
        starters = [spawn(application, first, 'race', race_scope) for _ in range(4)]
        for child, _ in starters:
            child.stdin.write(b'g')
        states = [next_line(lines) for _, lines in starters]
        if states.count('ready') != 1 or states.count('forwarded') != 3:
            raise RuntimeError('Unexpected concurrent startup results: ' + repr(states))
        leader, leader_lines = starters[states.index('ready')]
        forwarded_exits = [child.wait() for (child, _), state in zip(starters, states) if state == 'forwarded']
        race_received = [next_line(leader_lines) for _ in range(3)]
        leader.stdin.write(b'x')
        leader_exit = leader.wait()
        for child, _ in starters:
            child.close()
        delayed_scope = (Path(scope).parent / ('xdnote-delay-' + uuid.uuid4().hex)).as_posix()
        gate_name = 'XDNOTE_IPC_TEST_BOOTSTRAP_GATE'
        previous_gate = os.environ.get(gate_name)
        os.environ[gate_name] = 'gate'
        try:
            delayed, delayed_lines, state = start(application, first, 'server', delayed_scope)
            if state != 'bootstrap_ready':
                raise RuntimeError('Bootstrap gate was not reached: ' + state)
            early, early_lines, state = start(application, first, 'client', delayed_scope)
            if state != 'client_connected':
                raise RuntimeError('Early client did not connect to the bootstrap: ' + state)
            delayed.stdin.write(b'g')
            if next_line(delayed_lines) != 'ready':
                raise RuntimeError('Delayed product listener failed to start')
            early_exit = early.wait()
            delayed_received = next_line(delayed_lines)
            early.close()
            delayed.stdin.write(b'x')
            delayed_exit = delayed.wait()
            delayed.close()
        finally:
            if previous_gate is None:
                os.environ.pop(gate_name, None)
            else:
                os.environ[gate_name] = previous_gate
        result = {'preclaimed': preclaimed, 'preclaimed_still_available': still_available,
                  'preclaim_owner_exit': owner_exit, 'other_open': other_open,
                  'sequential': sequential, 'concurrent': concurrent, 'received': received,
                  'server_exit': server_exit, 'lock_cleared': not (Path(scope) / 'instance.lock').exists(),
                  'race_primary_count': states.count('ready'), 'race_forwarded_exits': forwarded_exits,
                  'race_received': race_received, 'race_leader_exit': leader_exit,
                  'race_lock_cleared': not (Path(race_scope) / 'instance.lock').exists(),
                  'early_client_exit': early_exit, 'early_received': delayed_received,
                  'delayed_server_exit': delayed_exit,
                  'delayed_lock_cleared': not (Path(delayed_scope) / 'instance.lock').exists()}
        expected = {'preclaimed': {'state': 'primary_failed', 'exit': 4}, 'preclaimed_still_available': True,
                    'preclaim_owner_exit': 0, 'other_open': 102, 'sequential': [0, 0], 'concurrent': [0, 0, 0, 0],
                    'received': ['accepted'] * 6, 'server_exit': 0, 'lock_cleared': True,
                    'race_primary_count': 1, 'race_forwarded_exits': [0] * 3,
                    'race_received': ['accepted'] * 3, 'race_leader_exit': 0, 'race_lock_cleared': True,
                    'early_client_exit': 0, 'early_received': 'accepted',
                    'delayed_server_exit': 0, 'delayed_lock_cleared': True}
        if result != expected:
            raise RuntimeError('Unexpected product IPC result: ' + json.dumps(result))
    finally:
        for child in reversed(children):
            child.close()
        failures = []
        for name, sid in reversed(profiles):
            native.a.FreeSid(sid)
            status = native.u.DeleteAppContainerProfile(name)
            if status:
                status = native.u.DeleteAppContainerProfile(name)
            if status:
                failures.append(name)
        if runtime.is_symlink() or runtime.resolve().parent != base or not runtime.name.startswith('product-isolation-'):
            raise RuntimeError('Refusing to remove an unexpected runtime directory')
        shutil.rmtree(runtime)
        if failures:
            raise RuntimeError('Temporary container profiles could not be removed: ' + repr(failures))
    print(json.dumps({'result': 'passed', 'profiles_deleted': True, **result}))

if __name__ == '__main__':
    main()
