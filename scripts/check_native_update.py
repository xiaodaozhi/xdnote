"""Build and exercise private macOS updates with ephemeral trust and owned data.

No production key or Keychain is read. This diagnostic creates local detached
fixture commits, never pushes them, and runs apps only via desktop.py run.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import plistlib
import secrets
import shutil
import socketserver
import subprocess
import threading
import time
import xml.etree.ElementTree as ET
import desktop
import sparkle
from native_fixture_proxy import Proxy, HOST
from installer_common import ROOT,build_output,digest
from release_candidate import TARGETS

BUNDLE_ID='io.github.xiaodaozhi.xdnote.engineering.nativefixture'
TEMPLATES=ROOT/'experiments/native-update-fixture'
OWNER='xdnote-private-native-update-v1'


def dump(path,value):path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def replace(path,old,new):
    text=path.read_text(encoding='utf-8')
    if text.count(old)!=1:raise ValueError('Private fixture patch no longer matches: '+path.name)
    path.write_text(text.replace(old,new),encoding='utf-8')


def checked(args,*,env=None,log=None,input=None):
    result=subprocess.run(list(map(str,args)),env=env,input=input,capture_output=True,timeout=900)
    if log:log.write_bytes(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError('Diagnostic command failed: '+Path(str(args[0])).name+'; see bounded evidence')
    return result


def source_tree(work,qt,env):
    if checked(['git','-C',ROOT,'status','--porcelain']).stdout.strip():raise ValueError('Commit current fixture inputs before native verification')
    revision=checked(['git','-C',ROOT,'rev-parse','HEAD']).stdout.decode().strip()
    source=work/'source';checked(['git','-C',ROOT,'worktree','add','--detach',source,revision],log=work/'worktree.log')
    prepared=source/'build';prepared.mkdir()
    shutil.copytree(ROOT/'build/scintilla-5.6.7',prepared/'scintilla-5.6.7')
    sdk=sparkle.BASE/'arm64';sparkle.verify(sdk,built=True)
    shutil.copytree(sdk,prepared/'Sparkle'/sparkle.configuration()['framework_build']['variant']/'arm64',symlinks=True)
    # Same-project worktree ownership is checked before sharing tools and SDKs.
    if desktop.repository_owner(source)[1]!=desktop.repository_owner(ROOT)[1]:raise ValueError('Fixture worktree belongs to another project')
    replace(source/'scripts/desktop.py','str(BUILD / "cargo-home")',repr(str(ROOT/'build/cargo-home')))
    replace(source/'CMakeLists.txt','${PROJECT_SOURCE_DIR}/build/cargo-home',str(ROOT/'build/cargo-home'))
    fixture=source/'experiments/native-update-fixture';run=work/'run';run.mkdir()
    (fixture/'Fixture.mm').write_text((TEMPLATES/'Fixture.mm.in').read_text().replace('@DIRECTORY@',json.dumps(str(run),ensure_ascii=False)),encoding='utf-8')
    replace(source/'CMakeLists.txt','    target_sources(xdnote_desktop PRIVATE apps/desktop/src/MacUpdateProvider.mm)',
        '    target_sources(xdnote_desktop PRIVATE apps/desktop/src/MacUpdateProvider.mm experiments/native-update-fixture/Fixture.mm)\n    target_include_directories(xdnote_desktop PUBLIC experiments/native-update-fixture)\n    set_source_files_properties(experiments/native-update-fixture/Fixture.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")')
    replace(source/'apps/desktop/main.cpp','#include "BuildInfo.h"','#include "BuildInfo.h"\n#include "Fixture.h"')
    replace(source/'apps/desktop/main.cpp','    if (!installation.allowed()) {','    fixtureBoot(&application); return application.exec();\n    if (!installation.allowed()) {')
    controller=source/'apps/desktop/src/UpdateController.cpp'
    replace(controller,'#include "UpdateDownload.h"','#include "UpdateDownload.h"\n#include "Fixture.h"')
    replace(controller,': UpdateController(nullptr, BuildInfo::version()',': UpdateController(fixtureNetwork(parent), BuildInfo::version()')
    replace(controller,'m_attemptPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates/attempt.json";','m_attemptPath = fixtureRoot() + "/../../attempt.json";')
    replace(controller,'QString UpdateController::installedRoot() const {','QString UpdateController::installedRoot() const { return fixtureRoot();')
    provider=source/'apps/desktop/src/MacUpdateProvider.mm'
    replace(provider,'#include "MacUpdateProvider.h"','#include "MacUpdateProvider.h"\n#include "Fixture.h"')
    replace(provider,'return native(UpdateNetwork::mapped(original, transport.mirror).toString());','return native(fixtureFeed());')
    replace(provider,'request.URL = valid ? [NSURL URLWithString:native(mapped.toString())]','request.URL = valid ? [NSURL URLWithString:native(fixtureArchive())]')
    text=provider.read_text();text=text.replace('    (void)error;\n    cancelDownload = nil;', '    fixtureNativeError(text(error.domain), error.code, text(error.localizedDescription), text(error.localizedFailureReason));\n    cancelDownload = nil;');provider.write_text(text)
    launcher=source/'scripts/desktop.py'
    replace(launcher,'    parser.add_argument("--fresh",','    parser.add_argument("--fixture-bundle", type=Path, help="private native update diagnostic only")\n    parser.add_argument("--fresh",')
    replace(launcher,'        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)', '''        if args.fixture_bundle:
            bundle = build_path(args.fixture_bundle)
            marker = json.loads((bundle.parent / '.fixture-allowed.json').read_text())
            executable = bundle / 'Contents/MacOS/xdnote'
            if marker.get('owner') != 'xdnote-private-native-update-v1' or hashlib.sha256(executable.read_bytes()).hexdigest() not in marker.get('allowedExecutableSha256', []):
                raise ValueError('Private fixture identity differs')
            env['QT_PLUGIN_PATH'] = str(bundle / 'Contents/PlugIns')
            env['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(bundle / 'Contents/PlugIns/platforms')
            env['QT_QPA_PLATFORM'] = 'cocoa'
        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)''')
    examples=source/'crates/update/examples';examples.mkdir(exist_ok=True)
    shutil.copyfile(TEMPLATES/'fixture_sign.rs.in',examples/'fixture_sign.rs')
    seed=run/'private-test-seed';seed.write_bytes(secrets.token_bytes(32));seed.chmod(0o600)
    signer_dir=work/'signer'
    checked(['rustup','run','1.99.0','cargo','build','--locked','--offline','--manifest-path',source/'Cargo.toml','-p','xdnote-update','--example','fixture_sign','--target-dir',signer_dir],env=env,log=work/'signer.log')
    signer=signer_dir/'debug/examples/fixture_sign'
    public=checked([signer,'public',seed]).stdout.decode().strip()
    dump(source/'tools/update-trust.json',{'schema_version':1,'keys':[{'key_id':'isolated-native-fixture','public_key':public}]})
    dump(source/'tools/sparkle-update-key.json',{'schemaVersion':1,'publicKey':base64.b64encode(bytes.fromhex(public)).decode()})
    dump(run/'settings.json',{'schemaVersion':1,'fixtureUnknown':'retained','updates':{'proxyMode':'none','proxyAddress':'','mirror':''}})
    (run/'owned.md').write_text('owned\n')
    checked(['git','-C',source,'add','CMakeLists.txt','apps/desktop','crates/update/examples','experiments/native-update-fixture','scripts/desktop.py','tools/update-trust.json','tools/sparkle-update-key.json'])
    checked(['git','-C',source,'-c','commit.gpgsign=false','commit','-m','test: isolate native update fixture and ephemeral trust'])
    identities={}
    for label,version,build,previous,previous_build in [('old','0.1.0-alpha.1',200,'0.0.0',0),('new','0.1.0-alpha.2',201,'0.1.0-alpha.1',200)]:
        if label=='new':checked(['git','-C',source,'-c','commit.gpgsign=false','commit','--allow-empty','-m','test: identify native update target build 201'])
        commit=checked(['git','-C',source,'rev-parse','HEAD']).stdout.decode().strip()
        record=prepared/(label+'-identity.json');dump(record,{'schemaVersion':1,'product':'xdnote','version':version,'build':build,'revision':commit,'previousVersion':previous,'previousBuild':previous_build})
        checked([os.sys.executable,launcher,'build','--profile','default','--build-dir',prepared/'desktop','--qt-prefix',qt,'--release-identity',record,'--parallel','4'],env=env,log=work/(label+'-build.log'))
        checked([os.sys.executable,source/'scripts/product_deployment_unix.py','--build-dir',prepared/'desktop','--qt-prefix',qt,'--destination',prepared/(label+'-product')],env=env,log=work/(label+'-deployment.log'))
        identities[label]=json.loads((prepared/'desktop/generated/build-version.json').read_bytes())
    return source,run,sdk,signer,identities,revision


def bundle(source,destination):
    if destination.exists():shutil.rmtree(destination)
    shutil.copytree(source,destination,symlinks=True)
    info_path=destination/'Contents/Info.plist';info=plistlib.loads(info_path.read_bytes())
    info.update(CFBundleIdentifier=BUNDLE_ID,NSAppTransportSecurity={'NSAllowsLocalNetworking':True,'NSExceptionDomains':{HOST:{'NSExceptionAllowsInsecureHTTPLoads':True}}})
    if not info.get('SURequireSignedFeed') or not info.get('SUVerifyUpdateBeforeExtraction') or info.get('SUSignedFeedFailureExpirationInterval')!=0:raise ValueError('Native authentication policy missing from product deployment')
    info_path.write_bytes(plistlib.dumps(info));checked(['/usr/bin/codesign','--force','--sign','-',destination]);checked(['/usr/bin/codesign','--verify','--deep','--strict',destination])


def candidate(source,run,sdk,signer,identity):
    new=run/'new/xdnote.app';new.parent.mkdir()
    bundle(source/'build/new-product/xdnote.app',new)
    archive=run/'archive.app.zip';checked(['/usr/bin/ditto','-c','-k','--sequesterRsrc','--keepParent',new,archive])
    seed=base64.b64encode((run/'private-test-seed').read_bytes())
    signature=checked([sdk/'bin/sign_update','--ed-key-file','-','-p',archive],input=seed).stdout.decode().strip()
    version=identity['version'];filename='xdnote-'+version+'-macos-arm64.app.zip';base='https://github.com/xiaodaozhi/xdnote/releases/download/v'+version+'/'
    ET.register_namespace('sparkle','http://www.andymatuschak.org/xml-namespaces/sparkle');ns='{http://www.andymatuschak.org/xml-namespaces/sparkle}'
    rss=ET.Element('rss',version='2.0');channel=ET.SubElement(rss,'channel');ET.SubElement(channel,'title').text='Isolated xdnote engineering fixture';item=ET.SubElement(channel,'item');ET.SubElement(item,'title').text=version
    for key,value in [('version',str(identity['build'])),('shortVersionString',version),('minimumSystemVersion','14.0'),('channel','alpha')]:ET.SubElement(item,ns+key).text=value
    ET.SubElement(item,'enclosure',{'url':base+filename,'length':str(archive.stat().st_size),'type':'application/octet-stream',ns+'edSignature':signature})
    feed=run/'appcast.xml';feed.write_bytes(ET.tostring(rss,encoding='utf-8',xml_declaration=True));checked([sdk/'bin/sign_update','--ed-key-file','-','--disable-signing-warning',feed],input=seed)
    assets=[{'filename':'xdnote-'+version+'-'+suffix,'platform':platform,'architecture':arch,'package_type':package,'minimum_system':minimum,'bytes':archive.stat().st_size,'sha256':digest(archive),'url':base+'xdnote-'+version+'-'+suffix} for platform,arch,package,suffix,minimum in TARGETS]
    dump(run/'release.json',{'schema_version':1,'product':'xdnote','version':version,'channel':'alpha','build':201,'tag':'v'+version,'commit':identity['revision'],'repository':'xiaodaozhi/xdnote','assets':assets,'macos_appcast_url':base+'appcast.xml'})
    checked([signer,'sign',run/'private-test-seed',run/'release.json',run/'release.json.sig'])
    dump(run/'listing.json',[{'tag_name':'v'+version,'draft':False,'assets':[{'name':filename,'size':archive.stat().st_size,'browser_download_url':base+filename}]+[{'name':name,'browser_download_url':base+name} for name in ['release.json','release.json.sig']]}])
    shutil.copy2(archive,run/'archive-valid.app.zip')
    (run/'private-test-seed').unlink() # No signing authority is present during execution.


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        self.server.requests.append({'path':self.path,'clientPort':self.client_address[1]})
        name={'/appcast.xml':'appcast.xml','/archive.app.zip':'archive.app.zip'}.get(self.path)
        if not name:self.send_error(404);return
        with (self.server.directory/name).open('rb') as source:
            self.send_response(200);self.send_header('Content-Length',str(os.fstat(source.fileno()).st_size));self.end_headers()
            while data:=source.read(65536):
                try:self.wfile.write(data)
                except (BrokenPipeError,ConnectionResetError):break
                if name=='archive.app.zip' and self.server.disconnect_archive:
                    self.close_connection=True;break


def launch(source,run,qt,env,mode,server,transport='none',proxy=None):
    install=source/'build/native-run/installed/xdnote.app';bundle(source/'build/old-product/xdnote.app',install)
    before=digest(install/'Contents/MacOS/xdnote');dump(install.parent/'.fixture-allowed.json',{'owner':OWNER,'allowedExecutableSha256':[before]})
    version='0.1.0-alpha.2';base=f'https://github.com/xiaodaozhi/xdnote/releases/download/v{version}/'
    address=f'{transport}://127.0.0.1:{proxy.server_address[1]}' if proxy else ''
    settings=json.loads((run/'settings.json').read_bytes());settings['updates']={'proxyMode':'custom' if proxy else transport,'proxyAddress':address,'mirror':''};dump(run/'settings.json',settings)
    host=HOST if proxy else '127.0.0.1'
    dump(run/'configuration.json',{'mode':mode,'server':f'http://{host}:{server.server_address[1]}/','routes':{'https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=1':'listing.json',base+'release.json':'release.json',base+'release.json.sig':'release.json.sig'}})
    server.disconnect_archive=mode=='network';start=len(server.requests);proxy_start=len(proxy.records) if proxy else 0
    (run/'owned.md').write_text('owned\n')
    unrelated={'id':'11111111-1111-4111-8111-111111111111','path':str(run/'unrelated.md'),'root':'','content':base64.b64encode(b'owned unrelated recovery\n').decode()}
    dump(run/'settings.json.recovery',{'schemaVersion':1,'documents':[],'drafts':[unrelated] if mode=='discard' else []})
    for name in ['error.json','status.json','native-error.json','cancelled.json','restarted.json','corrupt.json','recovery.json']:(run/name).unlink(missing_ok=True)
    pending=install.parent.parent/'attempt.json'
    if mode=='recovery':dump(pending,{'schema_version':1,'transaction':'a'*32,'phase':'committed','version':version,'build':201,'commit':'b'*40,'root':str(install),'executable':str(install/'Contents/MacOS/xdnote')})
    else:pending.unlink(missing_ok=True)
    shutil.copy2(run/'archive-valid.app.zip',run/'archive.app.zip')
    if mode=='corrupt':
        path=run/'archive.app.zip'
        with path.open('r+b') as stream:
            stream.seek(path.stat().st_size//2);byte=stream.read(1);stream.seek(-1,1);stream.write(bytes([byte[0]^1]))
    child_env=env.copy();child_env.update(CFFIXED_USER_HOME=str(run/'home'),XDG_CONFIG_HOME=str(run/'config'),XDG_CACHE_HOME=str(run/'cache'))
    command=[os.sys.executable,source/'scripts/desktop.py','run','--profile','default','--build-dir',source/'build/desktop','--qt-prefix',qt,'--fixture-bundle',install]
    try:checked(command,env=child_env,log=run/(transport+'-'+mode+'.log'))
    finally:dump(run/(transport+'-'+mode+'-transport.json'),{'originRequests':server.requests[start:],'proxyConnections':proxy.records[proxy_start:] if proxy else [],'proxyErrors':proxy.errors if proxy else []})
    report=run/({'cancel':'cancelled','install':'restarted','discard':'restarted','network':'corrupt'}.get(mode,mode)+'.json');deadline=time.monotonic()+120
    while not report.exists() and time.monotonic()<deadline:time.sleep(.2)
    value=json.loads(report.read_bytes())
    if not value.get('ok'):raise RuntimeError('Native fixture assertion failed: '+mode)
    if mode in ('cancel','corrupt','recovery','network') and digest(install/'Contents/MacOS/xdnote')!=before:raise RuntimeError('Old fixture app unexpectedly replaced')
    if mode in ('corrupt','network'):
        error=json.loads((run/'native-error.json').read_bytes())
        value['nativeError']=error
    value['transport']={'mode':transport,'originRequests':server.requests[start:],'proxyConnections':proxy.records[proxy_start:] if proxy else []}
    if proxy and mode!='recovery':
        ports={r['originClientPort'] for r in proxy.records[proxy_start:] if r['accepted']}
        if not ports or not server.requests[start:] or any(r['clientPort'] not in ports for r in server.requests[start:]):raise RuntimeError('Native request escaped observed private proxy')
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--qt-prefix',type=Path,required=True);p.add_argument('--proxy-matrix',action='store_true');a=p.parse_args()
    if platform.system()!='Darwin' or platform.machine()!='arm64':p.error('Requires native macOS 14+ arm64')
    desktop.check_host(platform.system(),platform.machine(),platform.mac_ver()[0]);qt=desktop.checked_tool_path(a.qt_prefix,'Qt SDK',sdk=True);work=build_output(a.work_dir)
    if work.exists():p.error('Preserve an existing private fixture')
    work.mkdir(parents=True);env=desktop.environment(None);env['CARGO_NET_OFFLINE']='true'
    rustc=Path(checked(['rustup','which','--toolchain','1.99.0','rustc'],env=env).stdout.decode().strip())
    rustc=desktop.checked_tool_path(rustc,'Pinned Rust compiler')
    env['PATH']=str(rustc.parent)+os.pathsep+env.get('PATH','')
    trust={name:digest(ROOT/'tools'/name) for name in ['update-trust.json','sparkle-update-key.json']}
    result={'ok':False,'scenarios':{},'productionTrustUnchanged':False,'privateSeedRemoved':False,'releaseReady':False}
    try:
        source,run,sdk,signer,identities,revision=source_tree(work,qt,env);candidate(source,run,sdk,signer,identities['new'])
        result.update(sourceRevision=revision,fixtureIdentities=identities,privateSeedRemoved=not (run/'private-test-seed').exists(),templates={path.name:digest(path) for path in TEMPLATES.iterdir() if path.is_file()})
        server=Server(('127.0.0.1',0),Handler);server.directory=run;server.requests=[];server.disconnect_archive=False;threading.Thread(target=server.serve_forever,daemon=True).start()
        try:
            for mode in ['recovery','corrupt','cancel','install','discard']:
                result['scenarios'][mode]=launch(source,run,qt,env,mode,server);dump(work/'report.json',result);print(mode+' passed',flush=True)
            if a.proxy_matrix:
                result['scenarios']['system-install']=launch(source,run,qt,env,'install',server,'system');dump(work/'report.json',result);print('system-install passed',flush=True)
                for transport in ['http','socks5']:
                    proxy=Proxy(transport,server.server_address[1]);threading.Thread(target=proxy.serve_forever,daemon=True).start()
                    try:
                        for mode in ['network','cancel','install','discard']:
                            key=transport+'-'+mode;result['scenarios'][key]=launch(source,run,qt,env,mode,server,transport,proxy);dump(work/'report.json',result);print(key+' passed',flush=True)
                    finally:proxy.shutdown();proxy.server_close()
        finally:server.shutdown();server.server_close()
        if any(digest(ROOT/'tools'/name)!=sha for name,sha in trust.items()):raise ValueError('Production trust files changed')
        result.update(ok=True,productionTrustUnchanged=True,scope='actual private macOS updates with compiled ephemeral trust, local signed feed/archive, synthetic other-platform asset records and owned document/settings; no production, minimum-OS or Applications registration acceptance')
    except Exception as error:result['error']=str(error);raise
    finally:dump(work/'report.json',result)


if __name__=='__main__':main()
