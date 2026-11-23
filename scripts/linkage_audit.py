"""Attribute linker-selected Rust archive members to locked source materials.

Object selection is evidence of linked compilation units, not a claim that every
line/third-party component in those units survived optimization or dead stripping.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import os
import subprocess
from installer_common import ROOT, build_output, digest
from pe_binary_evidence import function_ranges, layout as pe_layout, pe_code_sections
from unix_binary_evidence import code_sections

ARCHIVE_MEMBER = r'(?:lib)?xdnote_services\.(?:a|lib)\(([^)]+)\)'


def selected_members(text):
    # Apple and GNU archive members; MSVC emits library:object in its map.
    result = set(re.findall(ARCHIVE_MEMBER, text))
    result.update(re.findall(r'\bxdnote_services(?:\.lib)?:([^\s]+)', text, re.IGNORECASE))
    if not result: raise ValueError('No Rust service archive members in link map')
    return sorted(result)


def classify(members, registry, toolchain_units=()):
    result=[]; by_name={r['name'].replace('-','_'): r for r in registry}
    verified_units = set(toolchain_units)
    for member in members:
        if re.fullmatch(r'[A-Za-z0-9_.-]+\.dll', member, re.IGNORECASE):
            result.append({'member': member, 'kind': 'PE import descriptor', 'name': member,
                           'materials': 'linker-selected import name; does not attribute the external DLL bytes or their obligations'})
            continue
        match=re.match(r'([A-Za-z0-9_]+)-([0-9a-f]+)\.', member)
        if not match: result.append({'member':member,'kind':'unclassified native/archive object'}); continue
        name=match[1]
        if (name, match[2]) in verified_units:
            source={'kind':'Rust toolchain source unit','name':name,'archiveHash':match[2],'materials':'pinned toolchain COPYRIGHT and licenses; source-level component review remains separate'}
        elif name in by_name:
            value=by_name[name]; source={'kind':'locked Cargo source','name':value['name'],'version':value['version'],'license':value['selectedLicense'],'licenseFile':value['licenseFile']}
        elif name.startswith('xdnote_'): source={'kind':'project code','license':'Apache-2.0'}
        else: source={'kind':'unclassified Rust source unit','name':name}
        result.append({'member':member,**source})
    return result


def macos_text_evidence(link_map, sections, symbols, binary):
    """Cross-check live text spans against the unstripped ARM64 Mach-O."""
    section = re.search(r'^\s*sectname __text\s+segname __TEXT\s+addr (0x[0-9a-f]+)'
                        r'\s+size (0x[0-9a-f]+)\s+offset (\d+)', sections, re.MULTILINE)
    mapped = re.search(r'^(0x[0-9a-f]+)[ \t]+(0x[0-9a-f]+)[ \t]+__TEXT[ \t]+__text$',
                       link_map, re.MULTILINE | re.IGNORECASE)
    if not section or not mapped or '# Arch: arm64\n' not in link_map:
        raise ValueError('Expected ARM64 link map and Mach-O text section')
    start, length, offset = (int(value, 0) for value in section.groups())
    if (start, length) != tuple(int(value, 16) for value in mapped.groups()) or offset + length > len(binary):
        raise ValueError('Link map text bounds differ from the Mach-O')

    objects, boundary, live = link_map.partition('# Symbols:\n')
    if not boundary:
        raise ValueError('Mac link map has no live symbol table')
    live = live.partition('# Dead Stripped Symbols:')[0]
    members = dict(re.findall(r'^\[\s*(\d+)\][^\n]*?' + ARCHIVE_MEMBER, objects, re.MULTILINE))
    binary_symbols = {(int(address, 16), name) for address, name in
                      re.findall(r'^([0-9a-f]+) [Tt] (.+)$', symbols, re.MULTILINE)}
    counts = {member: Counter() for member in sorted(set(members.values()))}
    spans = []
    for address, size, index, name in re.findall(
            r'^(0x[0-9a-f]+)[ \t]+(0x[0-9a-f]+)[ \t]+\[\s*(\d+)\][ \t]+(.+)$',
            live, re.MULTILINE | re.IGNORECASE):
        member = members.get(index)
        address, size = int(address, 16), int(size, 16)
        if not member or not size or not start <= address < start + length:
            continue
        if address + size > start + length:
            raise ValueError('Rust symbol span exceeds the Mach-O text section')
        spans.append((address, address + size))
        bucket = counts[member]
        bucket['mappedSymbols'] += 1
        bucket['mappedBytes'] += size
        if (address, name) in binary_symbols:
            bucket['confirmedSymbols'] += 1
            bucket['confirmedBytes'] += size
        else:
            suffix = re.fullmatch(r'(.+)\.llvm\.[0-9]+', name)
            if suffix and (address, suffix[1]) in binary_symbols:
                bucket['llvmSuffixMatchedSymbols'] += 1
                bucket['llvmSuffixMatchedBytes'] += size
    spans.sort()
    if not spans or any(previous[1] > current[0] for previous, current in zip(spans, spans[1:])):
        raise ValueError('Missing or overlapping Rust text spans')
    if not any(bucket['confirmedSymbols'] or bucket['llvmSuffixMatchedSymbols'] for bucket in counts.values()):
        raise ValueError('No mapped Rust text symbols match the Mach-O symbol table')
    rows = [dict(member=member, **{key: values[key] for key in
            ('mappedSymbols', 'mappedBytes', 'confirmedSymbols', 'confirmedBytes',
             'llvmSuffixMatchedSymbols', 'llvmSuffixMatchedBytes')})
            for member, values in counts.items()]
    return {'binarySha256': hashlib.sha256(binary).hexdigest(),
            'textSha256': hashlib.sha256(binary[offset:offset + length]).hexdigest(),
            'textBytes': length, 'members': rows}


def linux_text_section(path):
    headers = subprocess.check_output(['/usr/bin/readelf', '--wide', '--section-headers', path], text=True)
    section = re.search(r'^\s*\[\s*\d+\]\s+\.text\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+AX\s',
                        headers, re.MULTILINE)
    if not section:
        raise ValueError('ELF executable .text section missing')
    address, offset, length = (int(value, 16) for value in section.groups())
    return address, offset, length


def section_digest(data, offset, length):
    if length <= 0 or offset < 0 or offset + length > len(data):
        raise ValueError('Executable section escapes the binary')
    return hashlib.sha256(data[offset:offset + length]).hexdigest()


def linux_text_evidence(link_map, section, symbols, binary):
    start, offset, length = section
    _, marker, live = link_map.partition('Linker script and memory map')
    mapped = re.search(r'^\.text[ \t]+(0x[0-9a-f]+)[ \t]+(0x[0-9a-f]+)', live, re.MULTILINE)
    if not marker or not mapped or tuple(int(value, 16) for value in mapped.groups()) != (start, length):
        raise ValueError('GNU map .text bounds differ from the ELF')
    live = live[mapped.end():]
    live = re.split(r'^\.\S+', live, maxsplit=1, flags=re.MULTILINE)[0]
    by_address = {(int(address, 16), int(size, 16), name) for name, address, size in re.findall(
        r'^(\S+) [tTwW] ([0-9a-f]+) ([0-9a-f]+)$', symbols, re.MULTILINE)}
    rows = defaultdict(Counter)
    spans = []
    pattern = (r'^ \.text(?:\.(\S+))?[ \t\n]+(0x[0-9a-f]+)[ \t]+(0x[0-9a-f]+)'
               r'[ \t]+[^\n]*?' + ARCHIVE_MEMBER + r'[ \t]*$')
    for name, address, size, member in re.findall(pattern, live, re.MULTILINE):
        address, size = int(address, 16), int(size, 16)
        if not size:
            continue
        if not start <= address < address + size <= start + length:
            raise ValueError('GNU map Rust input section escapes ELF .text')
        spans.append((address, address + size))
        bucket = rows[member]
        bucket['mappedRanges'] += 1
        bucket['mappedBytes'] += size
        name = re.sub(r'^(unlikely|hot|startup|exit)\.', '', name)
        if (address, size, name) in by_address:
            bucket['confirmedRanges'] += 1
            bucket['confirmedBytes'] += size
    spans.sort()
    if not any(row['confirmedRanges'] for row in rows.values()) or any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError('ELF Rust input ranges overlap or lack symbol confirmation')
    return {'binarySha256': hashlib.sha256(binary).hexdigest(), 'textSha256': section_digest(binary, offset, length),
            'textBytes': length, 'members': text_rows(rows, ('mappedRanges', 'mappedBytes', 'confirmedRanges', 'confirmedBytes'))}


def text_rows(rows, keys):
    return [dict(member=member, **{key: counts[key] for key in keys}) for member, counts in sorted(rows.items())]


def windows_text_evidence(link_map, binary):
    base, sections, exception = pe_layout(binary)
    preferred = re.search(r'Preferred load address is ([a-fA-F0-9]+)', link_map)
    if not preferred or int(preferred[1], 16) != base:
        raise ValueError('MSVC map base differs from the PE')
    functions = function_ranges(binary, sections, exception)
    entries = defaultdict(set)
    rust = defaultdict(set)
    pattern = (r'^[ \t]*([0-9a-f]+):([0-9a-f]+)[ \t]+\S+[ \t]+([0-9a-f]+)'
               r'[ \t]+f[ \t]+(?:i[ \t]+)?(\S+)[ \t]*$')
    for index, offset, absolute, origin in re.findall(pattern, link_map, re.MULTILINE | re.IGNORECASE):
        index, offset, address = int(index, 16), int(offset, 16), int(absolute, 16) - base
        entries[address].add(origin)
        owner = re.fullmatch(r'xdnote_services(?:\.lib)?:([^\s]+\.(?:obj|o))', origin, re.IGNORECASE)
        if not owner:
            continue
        if not 0 < index <= len(sections):
            raise ValueError('Rust map symbol has no corresponding PE section')
        sec = sections[index - 1]
        if not sec.flags & 0x20000000 or sec.address + offset != address or not sec.contains(address, address + 1):
            raise ValueError('Rust map symbol disagrees with the file-backed PE code address')
        rust[owner[1]].add(address)

    rows = {}
    for member, addresses in rust.items():
        counts = Counter(mappedAddresses=len(addresses))
        for address in addresses:
            if len(entries[address]) != 1:
                counts['ambiguousAddresses'] += 1
            elif address in functions:
                counts['confirmedRanges'] += 1
                counts['confirmedBytes'] += functions[address] - address
            else:
                counts['mapOnlyAddresses'] += 1
        rows[member] = counts
    if not any(row['confirmedRanges'] for row in rows.values()):
        raise ValueError('No Rust map address matches an unambiguous PE exception range')
    return {'binarySha256': hashlib.sha256(binary).hexdigest(), 'codeSections': pe_code_sections(binary),
            'members': text_rows(rows, ('mappedAddresses', 'confirmedRanges', 'confirmedBytes', 'ambiguousAddresses', 'mapOnlyAddresses'))}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','output'): p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--deployed-binary', type=Path, help='Optional packaged executable to compare')
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
    link_map = path.read_text(encoding='utf-8', errors='backslashreplace')
    records=classify(selected_members(link_map),manifest['rust'],units)
    report={'ok':True,'linkMapSha256':digest(path),'buildIdentity':json.loads((build/'build-identity.json').read_bytes()),'members':records,
            'summary':dict(Counter(r['kind'] for r in records)),'licenseReviewComplete':False,'releaseReady':False}
    if target == 'aarch64-apple-darwin':
        binary = build / 'xdnote'
        sections = subprocess.check_output(['/usr/bin/otool', '-l', binary]).decode('utf-8', errors='backslashreplace')
        symbols = subprocess.check_output(['/usr/bin/nm', '-an', binary]).decode('utf-8', errors='backslashreplace')
        report['macosTextEvidence'] = macos_text_evidence(link_map, sections, symbols, binary.read_bytes())
        if a.deployed_binary:
            deployed = build_output(a.deployed_binary).read_bytes()
            deployed_text = [value for name, value in code_sections(deployed, architecture='arm64').items()
                             if name.endswith(':__TEXT/__text')]
            if deployed_text != [report['macosTextEvidence']['textSha256']]:
                raise ValueError('Packaged Mach-O text differs from the linked executable')
            report['macosTextEvidence']['deployedBinarySha256'] = hashlib.sha256(deployed).hexdigest()
    elif target == 'x86_64-unknown-linux-gnu':
        binary = build / 'xdnote'
        symbols = subprocess.check_output(['/usr/bin/nm', '--defined-only', '--format=posix', '--print-size', binary], text=True, errors='backslashreplace')
        evidence = linux_text_evidence(link_map, linux_text_section(binary), symbols, binary.read_bytes())
        if a.deployed_binary:
            deployed_path = build_output(a.deployed_binary)
            deployed = deployed_path.read_bytes()
            _, offset, size = linux_text_section(deployed_path)
            if section_digest(deployed, offset, size) != evidence['textSha256']:
                raise ValueError('Packaged ELF .text differs from the linked executable')
            evidence['deployedBinarySha256'] = hashlib.sha256(deployed).hexdigest()
        report['linuxTextEvidence'] = evidence
    elif target == 'x86_64-pc-windows-msvc':
        evidence = windows_text_evidence(link_map, (build / 'xdnote.exe').read_bytes())
        if a.deployed_binary:
            deployed = build_output(a.deployed_binary).read_bytes()
            if pe_code_sections(deployed) != evidence['codeSections']:
                raise ValueError('Packaged PE code sections differ from the linked executable')
            evidence['deployedBinarySha256'] = hashlib.sha256(deployed).hexdigest()
        report['windowsTextEvidence'] = evidence
    elif a.deployed_binary:
        p.error('Packaged code comparison unavailable for this target')
    build_output(a.output).write_text(json.dumps(report,indent=2)+'\n')
