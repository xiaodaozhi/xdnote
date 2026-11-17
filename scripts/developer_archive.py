"""Create a sealed local developer archive; never publishes a Release."""
import argparse
import io
import json
import os
import platform
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
import zipfile

from installer_common import ROOT, build_output, digest
from product_deployment_unix import inventory
from release_policy import current_policy


def verify(root, metadata):
    if metadata.get('ok') is not True or metadata.get('files') != inventory(root): raise ValueError('Payload differs from sealed inventory')
    for item in metadata['files']:
        path=Path(item['path']); lower=path.as_posix().lower()
        if path.is_absolute() or '..' in path.parts or any(p in ('src','include','mkspecs','cmake','qml') for p in path.parts) or lower.endswith(('.h','.hpp','.cpp','.cxx','.cc','.c','.qml','.a','.lib','.pri','.prl','.cmake','.zip','.7z','.rar','.tar','.xz','.gz','.bz2','.zst','.tgz','.txz')):
            raise ValueError('Source/SDK/archive file cannot enter product archive: '+item['path'])


def write_archive(root, metadata, output):
    if str(output).endswith('.zip'):
        with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as bundle:
            for item in metadata['files']:
                if 'link' in item: raise ValueError('Use tar.xz for payloads with framework/SONAME links')
                info=zipfile.ZipInfo('xdnote/'+item['path'],date_time=(2020,1,1,0,0,0)); info.compress_type=zipfile.ZIP_DEFLATED
                info.create_system=3; info.external_attr=(stat.S_IFREG|0o644)<<16
                bundle.writestr(info,(root/item['path']).read_bytes(),compresslevel=6)
    elif str(output).endswith('.tar.xz'):
        with tarfile.open(output,'w:xz',preset=6) as bundle:
            for item in metadata['files']:
                source=root/item['path']; info=tarfile.TarInfo('xdnote/'+item['path']); info.mtime=0; info.uid=info.gid=0
                if 'link' in item:
                    info.type=tarfile.SYMTYPE; info.linkname=item['link']; info.mode=0o777; bundle.addfile(info)
                else:
                    info.mode=0o755 if source.stat().st_mode&0o111 else 0o644; info.size=item['bytes']
                    with source.open('rb') as stream: bundle.addfile(info,stream)
    else: raise ValueError('Expected .zip or .tar.xz developer archive')


def check_extraction(root, metadata, archive):
    with tempfile.TemporaryDirectory(prefix='archive-check-',dir=archive.parent) as directory:
        target=Path(directory).resolve()
        if str(archive).endswith('.zip'):
            with zipfile.ZipFile(archive) as bundle:
                if any(not (target/i.filename).resolve().is_relative_to(target) for i in bundle.infolist()): raise ValueError('Archive path escaped')
                bundle.extractall(target)
        else:
            with tarfile.open(archive,'r:xz') as bundle: bundle.extractall(target,filter='data')
        actual=inventory(target/'xdnote')
        if actual!=metadata['files']: raise ValueError('Extracted content differs from sealed product')
        if platform.system()=='Darwin' and (target/'xdnote/xdnote.app').exists():
            subprocess.run(['/usr/bin/codesign','--verify','--deep','--strict',str(target/'xdnote/xdnote.app')],check=True,timeout=30,capture_output=True)
        for item in metadata['files']:
            if 'link' not in item and (root/item['path']).stat().st_mode&0o111 and not (target/'xdnote'/item['path']).stat().st_mode&0o111 and not str(archive).endswith('.zip'):
                raise ValueError('Archive lost executable mode')


def build(root, metadata, compatibility, output):
    verify(root,metadata)
    native=set()
    for item in metadata['files']:
        if 'link' in item: continue
        with (root/item['path']).open('rb') as stream: magic=stream.read(4)
        if magic[:2]==b'MZ' or magic in (b'\x7fELF',b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca'): native.add(item['path'])
    if not native or not compatibility.get('ok') or {r['path'] for r in compatibility['files']}!=native or any(digest(root/r['path'])!=r['sha256'] for r in compatibility['files']): raise ValueError('Compatibility report is stale or incomplete')
    if output.exists(): raise ValueError('Preserve existing developer archive')
    output.parent.mkdir(parents=True,exist_ok=True)
    write_archive(root,metadata,output); check_extraction(root,metadata,output)
    with tempfile.TemporaryDirectory(prefix='archive-repeat-',dir=output.parent) as directory:
        repeated=Path(directory)/output.name; write_archive(root,metadata,repeated)
        if digest(repeated)!=digest(output): raise ValueError('Developer archive is not reproducible')
    verify(root,metadata)
    result={'schemaVersion':1,'ok':True,'archive':output.name,'sha256':digest(output),'bytes':output.stat().st_size,'payloadFiles':len(metadata['files']),
            'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
            'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()),
            'buildIdentity':metadata['buildIdentity'],'compatibilityScope':compatibility['scope'],'sourceCodeInRelease':False,
            'archiveReproducible':True,'extractedContentMatches':True,'releaseReady':False,'publicationAllowed':False,
            'distributionPolicy':current_policy(),
            'remaining':['source access and Qt replacement fulfillment review','actual Rust static component license scope','minimum-OS/real-desktop acceptance','explicit release authorization under the selected distribution policy']}
    output.with_name(output.name+'.sha256').write_text(result['sha256']+'  '+output.name+'\n',encoding='utf-8')
    output.with_name(output.name+'.manifest.json').write_text(json.dumps({'files':metadata['files'],**result},indent=2)+'\n',encoding='utf-8')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','audit','compatibility','output'): p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args(); print(json.dumps(build(build_output(a.root),json.loads(a.audit.read_text(encoding='utf-8')),json.loads(a.compatibility.read_text(encoding='utf-8')),build_output(a.output))))
