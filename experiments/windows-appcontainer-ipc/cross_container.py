"""Exercise the bootstrap DACL with two disposable, real AppContainer identities."""
import ctypes
from ctypes import wintypes
import json
import msvcrt
import os
from pathlib import Path
import queue
import subprocess
import threading
import uuid

k = ctypes.WinDLL('kernel32', use_last_error=True)
a = ctypes.WinDLL('advapi32', use_last_error=True)
u = ctypes.WinDLL('userenv', use_last_error=True)
u.CreateAppContainerProfile.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.LPVOID)]
u.CreateAppContainerProfile.restype = ctypes.c_long
u.DeleteAppContainerProfile.argtypes = [wintypes.LPCWSTR]
u.DeleteAppContainerProfile.restype = ctypes.c_long
a.FreeSid.argtypes = [wintypes.LPVOID]
a.EqualSid.argtypes = [wintypes.LPVOID, wintypes.LPVOID]
a.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
a.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
k.InitializeProcThreadAttributeList.argtypes = [wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)]
k.UpdateProcThreadAttribute.argtypes = [wintypes.LPVOID, wintypes.DWORD, ctypes.c_size_t, wintypes.LPVOID, ctypes.c_size_t, wintypes.LPVOID, wintypes.LPVOID]
k.DeleteProcThreadAttributeList.argtypes = [wintypes.LPVOID]
k.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.LPVOID, wintypes.LPVOID, wintypes.BOOL, wintypes.DWORD, wintypes.LPVOID, wintypes.LPCWSTR, wintypes.LPVOID, wintypes.LPVOID]
k.ResumeThread.argtypes = [wintypes.HANDLE]
k.ResumeThread.restype = wintypes.DWORD
k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
k.CloseHandle.argtypes = [wintypes.HANDLE]

class StartupInfo(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('reserved', wintypes.LPWSTR), ('desktop', wintypes.LPWSTR), ('title', wintypes.LPWSTR), ('x', wintypes.DWORD), ('y', wintypes.DWORD), ('xsize', wintypes.DWORD), ('ysize', wintypes.DWORD), ('xcount', wintypes.DWORD), ('ycount', wintypes.DWORD), ('fill', wintypes.DWORD), ('flags', wintypes.DWORD), ('show', wintypes.WORD), ('reserved2', wintypes.WORD), ('reservedptr', wintypes.LPBYTE), ('stdin', wintypes.HANDLE), ('stdout', wintypes.HANDLE), ('stderr', wintypes.HANDLE)]
class StartupInfoEx(ctypes.Structure):
    _fields_ = [('startup', StartupInfo), ('attributes', wintypes.LPVOID)]
class ProcessInformation(ctypes.Structure):
    _fields_ = [('process', wintypes.HANDLE), ('thread', wintypes.HANDLE), ('pid', wintypes.DWORD), ('tid', wintypes.DWORD)]
class SecurityCapabilities(ctypes.Structure):
    _fields_ = [('sid', wintypes.LPVOID), ('capabilities', wintypes.LPVOID), ('count', wintypes.DWORD), ('reserved', wintypes.DWORD)]

def check(success, operation):
    if not success:
        raise OSError(ctypes.get_last_error(), operation)

def token_information(token, kind):
    needed = wintypes.DWORD()
    a.GetTokenInformation(token, kind, None, 0, ctypes.byref(needed))
    data = ctypes.create_string_buffer(needed.value)
    check(a.GetTokenInformation(token, kind, data, needed, ctypes.byref(needed)), 'GetTokenInformation')
    return data

class Child:
    def __init__(self, executable, sid, mode, pipe, io=False):
        self.process = None
        self.stdin = self.stdout = None
        child_fds = []
        parent_fds = []
        attributes = None
        initialized = False
        try:
            if io:
                child_read, parent_write = os.pipe()
                parent_read, child_write = os.pipe()
                child_fds = [child_read, child_write]
                parent_fds = [parent_write, parent_read]
                for fd in child_fds:
                    os.set_handle_inheritable(msvcrt.get_osfhandle(fd), True)
            size = ctypes.c_size_t()
            k.InitializeProcThreadAttributeList(None, 2 if io else 1, 0, ctypes.byref(size))
            attributes = ctypes.create_string_buffer(size.value)
            check(k.InitializeProcThreadAttributeList(attributes, 2 if io else 1, 0, ctypes.byref(size)), 'InitializeProcThreadAttributeList')
            initialized = True
            caps = SecurityCapabilities(sid, None, 0, 0)
            check(k.UpdateProcThreadAttribute(attributes, 0, 0x20009, ctypes.byref(caps), ctypes.sizeof(caps), None, None), 'Update security capabilities')
            startup = StartupInfoEx()
            startup.startup.cb = ctypes.sizeof(startup)
            startup.attributes = ctypes.addressof(attributes)
            if io:
                handles = (wintypes.HANDLE * 2)(*(msvcrt.get_osfhandle(fd) for fd in child_fds))
                check(k.UpdateProcThreadAttribute(attributes, 0, 0x20002, handles, ctypes.sizeof(handles), None, None), 'Update inherited handle list')
                startup.startup.flags = 0x100
                startup.startup.stdin = handles[0]
                startup.startup.stdout = startup.startup.stderr = handles[1]
            pi = ProcessInformation()
            command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(executable), mode, pipe]))
            check(k.CreateProcessW(str(executable), command, None, None, io, 0x08080004, None, None, ctypes.byref(startup), ctypes.byref(pi)), 'CreateProcessW')
            self.process = pi.process
            try:
                token = wintypes.HANDLE()
                check(a.OpenProcessToken(pi.process, 8, ctypes.byref(token)), 'OpenProcessToken')
                try:
                    is_container = bool(ctypes.cast(token_information(token, 29), ctypes.POINTER(wintypes.DWORD))[0])
                    package = token_information(token, 31)
                    actual_sid = ctypes.cast(package, ctypes.POINTER(wintypes.LPVOID))[0]
                    if not is_container or not actual_sid or not a.EqualSid(actual_sid, sid):
                        raise RuntimeError('Child did not receive the requested container identity')
                finally:
                    k.CloseHandle(token)
                check(k.ResumeThread(pi.thread) != 0xffffffff, 'ResumeThread')
            finally:
                k.CloseHandle(pi.thread)
            if io:
                self.stdin = os.fdopen(parent_fds.pop(0), 'wb', buffering=0)
                self.stdout = os.fdopen(parent_fds.pop(0), 'rb', buffering=0)
        except BaseException:
            self.close()
            raise
        finally:
            for fd in child_fds + parent_fds:
                os.close(fd)
            if initialized:
                k.DeleteProcThreadAttributeList(attributes)

    def wait(self, milliseconds=10000):
        if k.WaitForSingleObject(self.process, milliseconds) != 0:
            raise RuntimeError('Child did not finish in time')
        code = wintypes.DWORD()
        check(k.GetExitCodeProcess(self.process, ctypes.byref(code)), 'GetExitCodeProcess')
        return code.value

    def close(self):
        if self.stdin:
            self.stdin.close()
            self.stdin = None
        if self.process:
            if k.WaitForSingleObject(self.process, 1000) != 0:
                k.TerminateProcess(self.process, 1)
                k.WaitForSingleObject(self.process, 5000)
            k.CloseHandle(self.process)
            self.process = None
        if self.stdout:
            self.stdout.close()
            self.stdout = None

def main():
    executable = Path.cwd() / 'build/cycle-268-appcontainer/out/container_isolation_probe.exe'
    if not executable.is_file():
        raise RuntimeError('Build the IPC experiment first')
    profiles = []
    children = []
    try:
        for _ in range(2):
            name = 'xdnote.ipc.test.' + uuid.uuid4().hex
            sid = wintypes.LPVOID()
            result = u.CreateAppContainerProfile(name, 'xdnote temporary IPC test', 'Disposable IPC test', None, 0, ctypes.byref(sid))
            if result:
                raise OSError(result, 'CreateAppContainerProfile')
            profiles.append((name, sid))
        if a.EqualSid(profiles[0][1], profiles[1][1]):
            raise RuntimeError('Test container identities are not distinct')
        def start_server(pipe):
            child = Child(executable, profiles[0][1], 'server', pipe, io=True)
            children.append(child)
            lines = queue.Queue()
            threading.Thread(target=lambda: lines.put(child.stdout.readline()), daemon=True).start()
            try:
                return child, lines.get(timeout=10)
            except queue.Empty:
                raise RuntimeError('Server did not report readiness')

        suffix = 'xdnote-isolation-' + uuid.uuid4().hex
        global_server, global_result = start_server('\\\\.\\pipe\\' + suffix)
        if global_result != b'pipe error 5\r\n' or global_server.wait() != 24:
            raise RuntimeError('Unexpected global pipe result: ' + repr(global_result))
        global_server.close()
        pipe = '\\\\.\\pipe\\LOCAL\\' + suffix
        server, ready = start_server(pipe)
        if ready != b'ready\r\n':
            raise RuntimeError('Server failed: ' + repr(ready))
        results = {}
        for label, identity, mode in [('other_open', 1, 'open'), ('other_instance', 1, 'instance'),
                                       ('same_instance', 0, 'instance'), ('same_open', 0, 'open')]:
            child = Child(executable, profiles[identity][1], mode, pipe)
            children.append(child)
            results[label] = child.wait()
            child.close()
        server.stdin.write(b'x')
        results['server_exit'] = server.wait()
        # Exit 102 represents ERROR_FILE_NOT_FOUND (2), not ERROR_ACCESS_DENIED.
        # The other identity creates its own same-named instance in its own namespace.
        if results != {'other_open': 102, 'other_instance': 0, 'same_instance': 0, 'same_open': 0, 'server_exit': 0}:
            raise RuntimeError('Unexpected isolation result: ' + json.dumps(results))
    finally:
        for child in reversed(children):
            child.close()
        failures = []
        for name, sid in reversed(profiles):
            a.FreeSid(sid)
            result = u.DeleteAppContainerProfile(name)
            if result:
                result = u.DeleteAppContainerProfile(name)
            if result:
                failures.append((name, result))
        if failures:
            raise RuntimeError('Could not remove temporary profiles: ' + repr(failures))
    print(json.dumps({'identities_distinct': True, 'global_server_error': 5, 'local_namespace': results,
                      'profiles_deleted': True, 'result': 'passed'}))

if __name__ == '__main__':
    main()
