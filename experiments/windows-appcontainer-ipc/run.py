import os, pathlib, queue, subprocess, sys, threading, uuid
from probe_environment import require_app_container
root=pathlib.Path.cwd();sys.path.insert(0,str(root/'scripts'))
from desktop import runtime_environment
require_app_container()
base=root/'build/cycle-268-appcontainer';base.mkdir(parents=True,exist_ok=True)
scope=base/('scope-'+uuid.uuid4().hex);scope.mkdir()
exe=base/'out/ipc_probe.exe';qt=root/'build/Qt/6.8.2/msvc2022_64'
env=runtime_environment(os.environ,qt)
try:
 p=subprocess.Popen([str(exe),'server',str(scope).replace('\\','/')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
 lines=queue.Queue();threading.Thread(target=lambda:lines.put(p.stdout.readline()),daemon=True).start()
 try: first=lines.get(timeout=15).strip()
 except queue.Empty: raise RuntimeError('Qt server did not report readiness within 15 seconds')
 print(first,flush=True)
 if first!='primary: ready':
  p.communicate(timeout=15);raise RuntimeError('Qt server failed to listen')
 out,_=p.communicate('x',timeout=45);print(out,end='')
 if p.returncode:raise RuntimeError('Qt multi-process forwarding failed: '+str(p.returncode))
finally:
 if 'p' in locals() and p.poll() is None:p.kill();p.wait()
 scope.rmdir()
