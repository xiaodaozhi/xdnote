import ctypes, json, uuid
from ctypes import wintypes
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.CreateNamedPipeW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,wintypes.DWORD,wintypes.DWORD,wintypes.DWORD,wintypes.DWORD,wintypes.LPVOID]
k.CreateNamedPipeW.restype=wintypes.HANDLE
k.CloseHandle.argtypes=[wintypes.HANDLE]
adv=ctypes.WinDLL('advapi32',use_last_error=True)
adv.OpenProcessToken.argtypes=[wintypes.HANDLE,wintypes.DWORD,ctypes.POINTER(wintypes.HANDLE)]
adv.GetTokenInformation.argtypes=[wintypes.HANDLE,ctypes.c_int,wintypes.LPVOID,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD)]
k.GetCurrentProcess.restype=wintypes.HANDLE
token=wintypes.HANDLE()
if not adv.OpenProcessToken(k.GetCurrentProcess(),8,ctypes.byref(token)):
 print(json.dumps({'step':'OpenProcessToken','winerror':ctypes.get_last_error()}));raise SystemExit(1)
tokens={}
for kind in (1,5,11,25,29,31):
 size=wintypes.DWORD();adv.GetTokenInformation(token,kind,None,0,ctypes.byref(size))
 buf=ctypes.create_string_buffer(size.value)
 if not adv.GetTokenInformation(token,kind,buf,size,ctypes.byref(size)):
  print(json.dumps({'step':'GetTokenInformation','kind':kind,'winerror':ctypes.get_last_error()}));raise SystemExit(1)
 tokens[kind]=buf
k.CloseHandle(token)
print(json.dumps({'probe':'restricted SID count','count':ctypes.cast(tokens[11],ctypes.POINTER(wintypes.DWORD))[0]}))
print(json.dumps({'probe':'AppContainer token','value':bool(ctypes.cast(tokens[29],ctypes.POINTER(wintypes.DWORD))[0])}))
integrity=ctypes.cast(tokens[25],ctypes.POINTER(ctypes.c_void_p))[0]
adv.GetSidSubAuthorityCount.argtypes=[wintypes.LPVOID];adv.GetSidSubAuthorityCount.restype=ctypes.POINTER(ctypes.c_ubyte)
adv.GetSidSubAuthority.argtypes=[wintypes.LPVOID,wintypes.DWORD];adv.GetSidSubAuthority.restype=ctypes.POINTER(wintypes.DWORD)
count=adv.GetSidSubAuthorityCount(integrity)[0]
rid=adv.GetSidSubAuthority(integrity,count-1)[0]
print(json.dumps({'probe':'integrity level category','category':'low' if rid<0x2000 else 'medium' if rid<0x3000 else 'high-or-higher'}))
user=ctypes.cast(tokens[1],ctypes.POINTER(ctypes.c_void_p))[0]
group=ctypes.cast(tokens[5],ctypes.POINTER(ctypes.c_void_p))[0]
adv.InitializeSecurityDescriptor.argtypes=[wintypes.LPVOID,wintypes.DWORD]
adv.InitializeAcl.argtypes=[wintypes.LPVOID,wintypes.DWORD,wintypes.DWORD]
adv.AddAccessAllowedAce.argtypes=[wintypes.LPVOID,wintypes.DWORD,wintypes.DWORD,wintypes.LPVOID]
adv.SetSecurityDescriptorOwner.argtypes=[wintypes.LPVOID,wintypes.LPVOID,wintypes.BOOL]
adv.SetSecurityDescriptorGroup.argtypes=[wintypes.LPVOID,wintypes.LPVOID,wintypes.BOOL]
adv.SetSecurityDescriptorDacl.argtypes=[wintypes.LPVOID,wintypes.BOOL,wintypes.LPVOID,wintypes.BOOL]
sd=ctypes.create_string_buffer(128); acl=ctypes.create_string_buffer(1024)
for name,call in [('InitializeSecurityDescriptor',lambda:adv.InitializeSecurityDescriptor(sd,1)),('InitializeAcl',lambda:adv.InitializeAcl(acl,1024,4)),('AddAccessAllowedAce',lambda:adv.AddAccessAllowedAce(acl,2,0x1f01ff,user)),('SetOwner',lambda:adv.SetSecurityDescriptorOwner(sd,user,False)),('SetGroup',lambda:adv.SetSecurityDescriptorGroup(sd,group,False)),('SetDacl',lambda:adv.SetSecurityDescriptorDacl(sd,True,acl,False))]:
 if not call(): print(json.dumps({'step':name,'winerror':ctypes.get_last_error()}));raise SystemExit(1)
class SA(ctypes.Structure): _fields_=[('length',wintypes.DWORD),('descriptor',wintypes.LPVOID),('inherit',wintypes.BOOL)]
sa=SA(ctypes.sizeof(SA),ctypes.addressof(sd),False)
name=r'\\.\pipe\xdnote-sandbox-probe-'+uuid.uuid4().hex
h=k.CreateNamedPipeW(name,0x40000003,0,255,4096,4096,0,None)
invalid=ctypes.c_void_p(-1).value
ok=h != invalid
error=0 if ok else ctypes.get_last_error()
print(json.dumps({'probe':'isolated unique CreateNamedPipeW with default security','success':ok,'winerror':error}))
if ok:
 second=k.CreateNamedPipeW(name,0x40000003,0,255,4096,4096,0,None)
 ok2=second != invalid; error2=0 if ok2 else ctypes.get_last_error()
 if ok2:k.CloseHandle(second)
 k.CloseHandle(h)
 print(json.dumps({'probe':'second instance with default security','success':ok2,'winerror':error2}))
name=r'\\.\pipe\xdnote-sandbox-probe-'+uuid.uuid4().hex
h=k.CreateNamedPipeW(name,0x40000003,0,255,0,0,3000,ctypes.byref(sa))
ok=h != invalid; error=0 if ok else ctypes.get_last_error()
print(json.dumps({'probe':'isolated CreateNamedPipeW with Qt-style user-only DACL','success':ok,'winerror':error}))
if ok:
 second=k.CreateNamedPipeW(name,0x40000003,0,255,0,0,3000,ctypes.byref(sa))
 ok2=second != invalid; error2=0 if ok2 else ctypes.get_last_error()
 if ok2:k.CloseHandle(second)
 k.CloseHandle(h)
 print(json.dumps({'probe':'second instance on same pipe with Qt-style user-only DACL','success':ok2,'winerror':error2}))
package=ctypes.cast(tokens[31],ctypes.POINTER(ctypes.c_void_p))[0]
if package:
 acl_user=ctypes.create_string_buffer(acl.raw,1024); sd_user=ctypes.create_string_buffer(128)
 if not adv.InitializeSecurityDescriptor(sd_user,1) or not adv.SetSecurityDescriptorOwner(sd_user,user,False) or not adv.SetSecurityDescriptorGroup(sd_user,group,False) or not adv.SetSecurityDescriptorDacl(sd_user,True,acl_user,False): raise OSError(ctypes.get_last_error(),'user-only descriptor')
 sa_user=SA(ctypes.sizeof(SA),ctypes.addressof(sd_user),False)
 if not adv.AddAccessAllowedAce(acl,2,0x1f01ff,package): raise OSError(ctypes.get_last_error(),'Add package ACE')
 name=r'\\.\pipe\xdnote-sandbox-probe-'+uuid.uuid4().hex
 first=k.CreateNamedPipeW(name,0x40000003,0,255,0,0,3000,ctypes.byref(sa))
 if first == invalid: raise OSError(ctypes.get_last_error(),'Create first package pipe')
 second=k.CreateNamedPipeW(name,0x40000003,0,255,0,0,3000,ctypes.byref(sa_user))
 ok2=second != invalid; error2=0 if ok2 else ctypes.get_last_error()
 if ok2:k.CloseHandle(second)
 k.CloseHandle(first)
 print(json.dumps({'probe':'Qt-style second instance after user and AppContainer bootstrap','success':ok2,'winerror':error2}))
