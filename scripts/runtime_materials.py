"""Preserve notices from the locked Rust toolchain and its selected crates."""
import json
from pathlib import Path
import shutil
import subprocess
from installer_common import ROOT, digest


def config():
    value=json.loads((ROOT/'tools/rust-runtime-materials.json').read_text(encoding='utf-8'))
    locked=json.loads((ROOT/'tools/desktop-dependencies.json').read_text(encoding='utf-8'))['rust']['toolchain']
    if value['version'] != locked: raise ValueError('Runtime material toolchain differs from lock')
    return value


def material_records(value):
    for record in value['files']:
        yield 'toolchain', Path(record['path']), record
    for package in value.get('crateNotices', []):
        for record in package['files']:
            yield 'vendored', Path('crates') / record['path'], record
    for package in value.get('toolchainSourceNotices', []):
        for record in package['files']:
            yield 'vendored', Path('toolchain-sources') / record['path'], record


def collect(destination):
    value=config(); destination=Path(destination)
    if destination.exists(): raise ValueError('Use a new runtime notice directory')
    command=['rustup','run',value['version'],'rustc']
    details=subprocess.check_output([*command,'-vV'],text=True)
    if 'commit-hash: '+value['compilerCommit'] not in details: raise ValueError('Rust compiler commit differs from material pin')
    sysroot=Path(subprocess.check_output([*command,'--print','sysroot'],text=True).strip()).resolve()
    sources={'toolchain':sysroot/'share/doc/rust', 'vendored':ROOT/'LICENSES/rust-runtime'}
    files=[]
    for kind,relative,record in material_records(value):
        base=sources[kind].resolve()
        source=base/record['path']
        if not source.resolve().is_relative_to(base) or digest(source)!=record['sha256']: raise ValueError('Rust runtime notice missing or changed: '+record['path'])
        files.append((source, relative))
    destination.mkdir(parents=True)
    for source,relative in files:
        target=destination/relative; target.parent.mkdir(parents=True,exist_ok=True); shutil.copyfile(source,target)
    receipt={**value,'rustc':details,'actualStaticComponentReviewComplete':False}
    (destination/'manifest.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    return verify(destination)


def verify(destination):
    destination=Path(destination); expected=config()
    receipt=json.loads((destination/'manifest.json').read_text(encoding='utf-8'))
    if any(receipt.get(k)!=v for k,v in expected.items()): raise ValueError('Runtime notice manifest changed')
    if 'commit-hash: '+expected['compilerCommit'] not in receipt.get('rustc',''): raise ValueError('Runtime compiler identity missing')
    materials = list(material_records(expected))
    wanted={'manifest.json',*(relative.as_posix() for _,relative,_ in materials)}
    found={p.relative_to(destination).as_posix() for p in destination.rglob('*') if p.is_file()}
    if found!=wanted or any(p.is_symlink() for p in destination.rglob('*')): raise ValueError('Unexpected or linked runtime notice')
    for _,relative,record in materials:
        if digest(destination/relative)!=record['sha256']: raise ValueError('Runtime notice digest differs: '+relative.as_posix())
    return receipt
