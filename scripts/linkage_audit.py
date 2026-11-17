"""Attribute linker-selected Rust archive members to locked source materials.

Object selection is evidence of linked compilation units, not a claim that every
line/third-party component in those units survived optimization or dead stripping.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import os
import subprocess
from installer_common import ROOT, build_output, digest


def selected_members(text):
    # Apple and GNU archive members; MSVC emits library:object in its map.
    result = set(re.findall(r'(?:lib)?xdnote_services\.(?:a|lib)\(([^)]+)\)', text))
    result.update(re.findall(r'xdnote_services\.lib:([^\s]+)', text, re.IGNORECASE))
    if not result: raise ValueError('No Rust service archive members in link map')
    return sorted(result)


def classify(members, registry, toolchain_units=()):
    result=[]; by_name={r['name'].replace('-','_'): r for r in registry}
    toolchain={'std','core','alloc','panic_unwind','panic_abort','compiler_builtins','unwind','rustc_std_workspace_core','rustc_std_workspace_alloc',
               'object','addr2line','gimli','rustc_demangle','hashbrown','memchr','libc','cfg_if','miniz_oxide','adler2','rustc_std_workspace_std','windows_link'}
    for member in members:
        match=re.match(r'([A-Za-z0-9_]+)-([0-9a-f]+)\.', member)
        if not match: result.append({'member':member,'kind':'unclassified native/archive object'}); continue
        name=match[1]
        if (name, match[2]) in toolchain_units:
            source={'kind':'Rust toolchain source unit','name':name,'archiveHash':match[2],'materials':'pinned toolchain COPYRIGHT and licenses; source-level component review remains separate'}
        elif name in by_name:
            value=by_name[name]; source={'kind':'locked Cargo source','name':value['name'],'version':value['version'],'license':value['selectedLicense'],'licenseFile':value['licenseFile']}
        elif name.startswith('xdnote_'): source={'kind':'project code','license':'Apache-2.0'}
        elif name in toolchain: source={'kind':'Rust toolchain source unit','name':name,'materials':'pinned toolchain COPYRIGHT and licenses; source-level component review remains separate'}
        else: source={'kind':'unclassified Rust source unit','name':name}
        result.append({'member':member,**source})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','output'): p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args(); build=build_output(a.build_dir); path=build/'xdnote-link.map'
    manifest=json.loads((ROOT/'LICENSES/third-party/manifest.json').read_bytes())
    sysroot=Path(subprocess.check_output(['rustup','run','1.99.0','rustc','--print','sysroot'],text=True,env=os.environ).strip())
    cache=(build/'CMakeCache.txt').read_text()
    target=re.search(r'^XDNOTE_RUST_TARGET:STRING=(.+)$',cache,re.MULTILINE)[1]
    units=[]
    for archive in (sysroot/'lib/rustlib'/target/'lib').glob('lib*.rlib'):
        match=re.fullmatch(r'lib([A-Za-z0-9_]+)-([0-9a-f]+)\.rlib',archive.name)
        if match: units.append(tuple(match.groups()))
    if not units: raise ValueError('Pinned toolchain archive identities unavailable')
    records=classify(selected_members(path.read_text(encoding='utf-8',errors='backslashreplace')),manifest['rust'],units)
    report={'ok':True,'linkMapSha256':digest(path),'buildIdentity':json.loads((build/'build-identity.json').read_bytes()),'members':records,
            'summary':dict(Counter(r['kind'] for r in records)),'licenseReviewComplete':False,'releaseReady':False}
    build_output(a.output).write_text(json.dumps(report,indent=2)+'\n')
