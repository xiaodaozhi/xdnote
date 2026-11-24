import ctypes, hashlib, os, pathlib, queue, runpy, subprocess, sys, threading, uuid
root=pathlib.Path.cwd();sys.path.insert(0,str(root/'scripts'))
from desktop import runtime_environment
probe=runpy.run_path(str(root/'experiments/windows-appcontainer-ipc/security_probe.py'))
if not probe['package']:raise RuntimeError('AppContainer required')
base=root/'build/cycle-268-appcontainer'; scope=base/('crash-scope-'+uuid.uuid4().hex);scope.mkdir()
scope_qt=str(scope).replace('\\','/')
name='xdnote-v1-'+hashlib.sha256(scope_qt.encode()).hexdigest()[:24]
pipe='\\\\.\\pipe\\'+name
k=probe['k'];invalid=ctypes.c_void_p(-1).value; handle=None; p=None
exe=base/'out/ipc_probe.exe'; env=runtime_environment(os.environ,root/'build/Qt/6.8.2/msvc2022_64')
def start():
 global handle,p
 handle=k.CreateNamedPipeW(pipe,0x40080003,0,255,0,0,3000,ctypes.byref(probe['sa']))
 if handle==invalid:handle=None;raise OSError(ctypes.get_last_error(),'bootstrap')
 p=subprocess.Popen([str(exe),'server',scope_qt],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
 lines=queue.Queue();threading.Thread(target=lambda:lines.put(p.stdout.readline()),daemon=True).start()
 try:line=lines.get(timeout=15).strip()
 except queue.Empty:raise RuntimeError('server readiness timeout')
 if line!='primary: ready':raise RuntimeError('server did not become primary: '+line)
 k.CloseHandle(handle);handle=None
try:
 start();p.kill();p.wait(timeout=10)
 print('owned server terminated; lock remains:',(scope/'instance.lock').is_file(),flush=True)
 if not (scope/'instance.lock').is_file():raise RuntimeError('no stale lock to exercise')
 start();out,_=p.communicate('x',timeout=45);print(out,end='')
 if p.returncode:raise RuntimeError('recovery client exercise failed')
 if (scope/'instance.lock').exists():raise RuntimeError('lock still exists after clean shutdown')
 print('stale lock recovered and normal shutdown cleaned it')
finally:
 if handle:k.CloseHandle(handle)
 if p and p.poll() is None:p.kill();p.wait()
 lock=scope/'instance.lock'
 if lock.exists():lock.unlink()
 scope.rmdir()
