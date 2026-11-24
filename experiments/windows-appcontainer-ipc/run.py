import ctypes, hashlib, os, pathlib, queue, runpy, subprocess, sys, threading, uuid
root=pathlib.Path.cwd();sys.path.insert(0,str(root/'scripts'))
from desktop import runtime_environment
probe=runpy.run_path(str(root/'experiments/windows-appcontainer-ipc/security_probe.py'))
if not probe['package']: raise RuntimeError('This experiment requires an AppContainer')
base=root/'build/cycle-268-appcontainer';base.mkdir(parents=True,exist_ok=True)
scope=base/('scope-'+uuid.uuid4().hex);scope.mkdir()
name='xdnote-v1-'+hashlib.sha256(str(scope).replace('\\','/').encode()).hexdigest()[:24]
pipe='\\\\.\\pipe\\'+name
k=probe['k'];invalid=ctypes.c_void_p(-1).value
handle=k.CreateNamedPipeW(pipe,0x40080003,0,255,0,0,3000,ctypes.byref(probe['sa']))
if handle==invalid:
 error=ctypes.get_last_error();scope.rmdir();raise OSError(error,'bootstrap CreateNamedPipeW')
duplicate=k.CreateNamedPipeW(pipe,0x40080003,0,255,0,0,3000,ctypes.byref(probe['sa']))
if duplicate!=invalid:
 k.CloseHandle(duplicate);k.CloseHandle(handle);scope.rmdir();raise RuntimeError('Duplicate first-instance claim unexpectedly succeeded')
if ctypes.get_last_error()!=5:
 k.CloseHandle(handle);scope.rmdir();raise RuntimeError('Unexpected duplicate first-instance failure')
print('duplicate first-instance claim rejected',flush=True)
exe=base/'out/ipc_probe.exe';qt=root/'build/Qt/6.8.2/msvc2022_64'
env=runtime_environment(os.environ,qt)
try:
 p=subprocess.Popen([str(exe),'server',str(scope).replace('\\','/')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
 lines=queue.Queue();threading.Thread(target=lambda:lines.put(p.stdout.readline()),daemon=True).start()
 try: first=lines.get(timeout=15).strip()
 except queue.Empty: raise RuntimeError('Qt server did not report readiness within 15 seconds')
 print(first,flush=True)
 if first!='primary: ready':
  p.communicate(timeout=15);raise RuntimeError('Qt server failed while bootstrap held')
 k.CloseHandle(handle);handle=None
 out,_=p.communicate('x',timeout=45);print(out,end='')
 if p.returncode:raise RuntimeError('Qt multi-process forwarding failed: '+str(p.returncode))
finally:
 if handle:k.CloseHandle(handle)
 if 'p' in locals() and p.poll() is None:p.kill();p.wait()
 scope.rmdir()
