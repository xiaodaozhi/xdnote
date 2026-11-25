"""Check read/write pipe access from two Windows users in the same AppContainer identity."""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import time

import cross_container as native


sid_pointer = ctypes.POINTER(wintypes.LPVOID)
native.a.GetLengthSid.argtypes = [wintypes.LPVOID]
native.a.GetLengthSid.restype = wintypes.DWORD
native.a.ConvertSidToStringSidW.argtypes = [wintypes.LPVOID, ctypes.POINTER(wintypes.LPWSTR)]
native.k.GetCurrentProcess.restype = wintypes.HANDLE
native.k.LocalFree.argtypes = [wintypes.LPVOID]
native.u.GetAppContainerFolderPath.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.LPWSTR)]
native.u.GetAppContainerFolderPath.restype = ctypes.c_long
ole = ctypes.WinDLL('ole32')
ole.CoTaskMemFree.argtypes = [wintypes.LPVOID]


def sid_hash(sid):
    return hashlib.sha256(ctypes.string_at(sid, native.a.GetLengthSid(sid))).hexdigest()


def token_user_hash(token):
    value = native.token_information(token, 1)
    return sid_hash(ctypes.cast(value, sid_pointer)[0])


def user_hash(process):
    token = wintypes.HANDLE()
    native.check(native.a.OpenProcessToken(process, 8, ctypes.byref(token)), 'OpenProcessToken')
    try:
        return token_user_hash(token)
    finally:
        native.k.CloseHandle(token)


@contextmanager
def profile(name):
    sid = wintypes.LPVOID()
    status = native.u.CreateAppContainerProfile(name, 'xdnote IPC test', 'Disposable IPC test', None, 0, ctypes.byref(sid))
    if status:
        raise OSError(status, 'CreateAppContainerProfile')
    try:
        yield sid
    finally:
        native.a.FreeSid(sid)
        if native.u.DeleteAppContainerProfile(name) and native.u.DeleteAppContainerProfile(name):
            raise RuntimeError('Could not delete temporary AppContainer profile')


def prepare_server(runtime, sid):
    text = wintypes.LPWSTR()
    native.check(native.a.ConvertSidToStringSidW(sid, ctypes.byref(text)), 'ConvertSidToStringSidW')
    try:
        for file in runtime.iterdir():
            if file.suffix in ('.exe', '.dll'):
                subprocess.run(['icacls', str(file), '/grant', '*' + text.value + ':(RX)'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        folder = wintypes.LPWSTR()
        status = native.u.GetAppContainerFolderPath(text, ctypes.byref(folder))
        if status:
            raise OSError(status, 'GetAppContainerFolderPath')
        try:
            return (Path(folder.value) / 'isolated-xdnote').as_posix()
        finally:
            ole.CoTaskMemFree(ctypes.cast(folder, wintypes.LPVOID))
    finally:
        native.k.LocalFree(ctypes.cast(text, wintypes.LPVOID))


def require_user(expected_user):
    def verify(token):
        if token_user_hash(token) != expected_user:
            raise RuntimeError('Child token does not belong to its expected account')
    return verify


def open_without_data(runtime, sid, pipe, expected_user):
    client = native.Child(runtime / 'container_isolation_probe.exe', sid, 'open', pipe,
                          verify_token=require_user(expected_user))
    try:
        return client.wait()
    finally:
        client.close()


def serve(runtime, sid, current_user):
    scope = prepare_server(runtime, sid)
    pipe = '\\\\.\\pipe\\LOCAL\\xdnote-v1-' + hashlib.sha256(scope.encode('utf-8')).hexdigest()[:24]
    server = native.Child(runtime / 'product_instance_probe.exe', sid, 'server', scope, io=True,
                          verify_token=require_user(current_user))
    try:
        line = queue.Queue()
        threading.Thread(target=lambda: line.put(server.stdout.readline()), daemon=True).start()
        if line.get(timeout=10).strip() != b'ready':
            raise RuntimeError('Product listener could not start')
        if open_without_data(runtime, sid, pipe, current_user) != 0:
            raise RuntimeError('First account could not open its own pipe')
        marker = runtime / 'endpoint.tmp'
        marker.write_text(json.dumps({'pipe': pipe, 'user': current_user, 'container': sid_hash(sid)}), encoding='ascii')
        marker.rename(runtime / 'endpoint.json')
        deadline = time.monotonic() + 25
        while not (runtime / 'stop').exists():
            if time.monotonic() >= deadline or native.k.WaitForSingleObject(server.process, 100) == 0:
                raise RuntimeError('Product listener stopped before the access check finished')
        if open_without_data(runtime, sid, pipe, current_user) != 0:
            raise RuntimeError('First account lost access during the check')
        server.stdin.write(b'x')
        if server.wait():
            raise RuntimeError('Product listener did not exit cleanly')
    finally:
        server.close()


def query(runtime, sid, current_user):
    endpoint = runtime / 'endpoint.json'
    deadline = time.monotonic() + 35
    while not endpoint.is_file():
        if time.monotonic() >= deadline:
            raise RuntimeError('No endpoint was published by the first account')
        time.sleep(.1)
    data = json.loads(endpoint.read_text(encoding='ascii'))
    if current_user == data['user'] or sid_hash(sid) != data['container']:
        raise RuntimeError('Expected different Windows users and the same AppContainer identity')
    result = open_without_data(runtime, sid, data['pipe'], current_user)
    # The client probe adds 100 to errors other than ACCESS_DENIED. FILE_NOT_FOUND
    # establishes isolation by visibility; it does not establish an ACL denial.
    return {5: 15, 102: 12}.get(result, 2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('serve', 'query'))
    parser.add_argument('runtime', type=Path)
    parser.add_argument('profile_name')
    args = parser.parse_args()
    if not args.profile_name.startswith('xdnote.ipc.account.'):
        parser.error('Expected a disposable test profile')
    current_user = user_hash(native.k.GetCurrentProcess())
    with profile(args.profile_name) as sid:
        if args.mode == 'query':
            return query(args.runtime, sid, current_user)
        serve(args.runtime, sid, current_user)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
