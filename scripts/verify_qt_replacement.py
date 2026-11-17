"""Run actual application services with an independently rebuilt Qt Core on macOS.

The supplied library must expose the explicit source-proof function. Only owned
build copies are changed; the product, fixed SDK and user installations stay intact.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import desktop
import deployment_unix as deploy
import product_deployment_unix as product
import user_install
from installer_common import build_output,digest
from unix_binary_evidence import code_sections


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('payload','audit','build-dir','qt-prefix','replacement','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if platform.system()!='Darwin':p.error('This source replacement probe currently requires macOS arm64')
    build=desktop.build_path(a.build_dir); qt=desktop.checked_tool_path(a.qt_prefix,'Qt SDK',sdk=True)
    identity=desktop.verify_build_identity(build,'default',qt)
    root=build_output(a.payload); audit=json.loads(a.audit.read_bytes()); output=build_output(a.output); replacement=build_output(a.replacement)
    if audit.get('files')!=product.inventory(root) or audit.get('buildIdentity')!=identity:raise ValueError('Current sealed product identity required')
    if output.exists():raise ValueError('Preserve existing replacement evidence')
    output.mkdir(parents=True); installed=output/'installed'
    user_install.install(root,installed,apply=True,registration=False)
    core=installed/'xdnote.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore'
    sdk_core=qt/'lib/QtCore.framework/Versions/A/QtCore'; sdk_hash=digest(sdk_core)
    original=digest(core); input_hash=digest(replacement)
    if code_sections(core.read_bytes())==code_sections(replacement.read_bytes()):raise ValueError('Replacement must contain actual source changes')
    if '_xdnoteQtSourceProof' not in deploy.inspect(['/usr/bin/nm','-g',replacement]):raise ValueError('Source proof symbol missing')
    shutil.copy2(replacement,core)
    _,rpaths,_=deploy.macho_loads(deploy.inspect(['/usr/bin/otool','-l',core]))
    for path in rpaths:deploy.inspect(['/usr/bin/install_name_tool','-delete_rpath',path,core])
    deploy.inspect(['/usr/bin/install_name_tool','-add_rpath','@loader_path/../../..',core])
    framework=installed/'xdnote.app/Contents/Frameworks/QtCore.framework'
    deploy.inspect(['/usr/bin/codesign','--force','--sign','-',framework])
    deploy.inspect(['/usr/bin/codesign','--force','--sign','-',installed/'xdnote.app'])
    deploy.inspect(['/usr/bin/codesign','--verify','--deep','--strict',installed/'xdnote.app'])
    os.environ['XDNOTE_EXPECT_QT_SOURCE_PROOF']='xdnote-qt-source-rebuild-6.8.2'
    # smoke creates only its own temporary documents and external offscreen probe.
    probes=product.smoke(installed,build,qt,output,'Darwin')
    if any(r.get('qtSourceProof')!=os.environ['XDNOTE_EXPECT_QT_SOURCE_PROOF'] for r in probes):raise ValueError('Actual modified library was not loaded')
    protections=[]
    for name,operation in [('upgrade',lambda:user_install.install(root,installed,apply=True,registration=False)),('uninstall',lambda:user_install.uninstall(installed,apply=True,registration=False))]:
        try:operation()
        except ValueError:protections.append(name)
        else:raise ValueError('Modified user library was overwritten or removed')
    if product.inventory(root)!=audit['files'] or digest(replacement)!=input_hash or digest(sdk_core)!=sdk_hash:raise ValueError('Source or original deployment changed')
    result={'ok':True,'buildIdentity':identity,'replacementInputSha256':input_hash,'originalQtCoreSha256':original,'actualQtCoreSha256':digest(core),'probes':probes,
            'modifiedLibraryProtected':protections,'fixedSdkChanged':False,'fixedSdkQtCoreSha256':sdk_hash,'licenseReviewComplete':False,'releaseReady':False,
            'scope':'actual modified QtCore loaded in owned developer installation; not all Qt modules, platforms or final source-obligation acceptance'}
    (output/'report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
