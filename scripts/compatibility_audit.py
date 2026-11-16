"""Inspect every delivered native file without executing it."""
import argparse
import json
from pathlib import Path
import platform
import re
import struct

from installer_common import ROOT, build_output, digest
from deployment_inventory import pe_imports
from deployment_unix import staged_files, inspect, elf_dynamic


def version(value):
    return tuple(int(part) for part in value.split('.'))


def at_most(actual, limit):
    size = max(len(actual), len(limit))
    return tuple(actual) + (0,) * (size - len(actual)) <= tuple(limit) + (0,) * (size - len(limit))


def pe_header(data):
    try:
        offset, = struct.unpack_from('<I', data, 0x3c)
        if data[:2] != b'MZ' or data[offset:offset + 4] != b'PE\0\0': raise ValueError('Invalid PE signature')
        machine, = struct.unpack_from('<H', data, offset + 4)
        width, = struct.unpack_from('<H', data, offset + 20)
        optional = offset + 24
        if width < 112 or optional + width > len(data) or struct.unpack_from('<H', data, optional)[0] != 0x20b: raise ValueError('Expected bounded PE32+')
        os_version = struct.unpack_from('<HH', data, optional + 40)
        subsystem = struct.unpack_from('<HH', data, optional + 48)
        return {'architecture': 'x86_64' if machine == 0x8664 else hex(machine), 'osVersion': list(os_version), 'subsystemVersion': list(subsystem)}
    except struct.error as error: raise ValueError('Truncated PE headers') from error


def elf_header(data):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01': raise ValueError('Expected ELF64 little endian')
    machine, = struct.unpack_from('<H', data, 18)
    return {'architecture': 'x86_64' if machine == 62 else hex(machine)}


def needed_versions(text):
    needed = False; result = set()
    for line in text.splitlines():
        if line.startswith('Version needs section'): needed = True
        elif re.match(r'Version \w+ section', line): needed = False
        if needed:
            result.update(re.findall(r'\bName:\s*((?:GLIBCXX|GLIBC|CXXABI)_[A-Za-z0-9_.]+)', line))
    return sorted(result)


def check_versions(requirements, policy):
    errors = []
    for item in requirements:
        if item in policy['specialVersions']: continue
        family, number = item.split('_', 1)
        if not re.fullmatch(r'\d+(?:\.\d+)*', number) or not at_most(version(number), policy['symbolVersions'][family]):
            errors.append(item)
    if errors: raise ValueError('ABI requirements exceed baseline or are unclassified: ' + ', '.join(errors))


def macho_headers(data):
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data): raise ValueError('Truncated Mach-O')
        return struct.unpack_from(fmt, data, offset)
    if data[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        endian = '>' if data[:4] == b'\xca\xfe\xba\xbe' else '<'
        count, = unpack(endian + 'I', 4)
        if not 1 <= count <= 16: raise ValueError('Invalid fat architecture count')
        result = []
        for index in range(count):
            cpu, _, start, size, _ = unpack(endian + 'IIIII', 8 + index * 20)
            if start + size > len(data): raise ValueError('Truncated fat slice')
            slices = macho_headers(data[start:start + size])
            if len(slices) != 1 or slices[0]['cpu'] != cpu: raise ValueError('Inconsistent fat CPU')
            result.extend(slices)
        if len({s['architecture'] for s in result}) != len(result): raise ValueError('Duplicate fat architectures')
        return result
    if data[:4] != b'\xcf\xfa\xed\xfe': raise ValueError('Expected Mach-O64')
    cpu, _, _, count, width = unpack('<IIIII', 4)
    architecture = {0x1000007:'x86_64', 0x100000c:'arm64'}.get(cpu, hex(cpu))
    end = 32 + width; offset = 32; minimum = []; dependencies = []
    if end > len(data) or count > 65535: raise ValueError('Invalid Mach-O command table')
    for _ in range(count):
        command, size = unpack('<II', offset)
        if size < 8 or offset + size > end: raise ValueError('Invalid Mach-O command size')
        if command == 0x32:
            if size < 24: raise ValueError('Truncated build-version command')
            target, encoded, _ = unpack('<III', offset + 8)
            if target != 1: raise ValueError('Non-macOS platform in product')
            minimum.append(encoded)
        elif command == 0x24:
            if size < 16: raise ValueError('Truncated minimum-version command')
            minimum.append(unpack('<I', offset + 8)[0])
        elif command in (0xc, 0x80000018, 0x8000001f, 0x80000023):
            if size < 24: raise ValueError('Truncated dylib command')
            start, = unpack('<I', offset + 8)
            if start < 24 or start >= size: raise ValueError('Invalid dylib string')
            raw = data[offset + start:offset + size]
            if b'\0' not in raw: raise ValueError('Unterminated dylib string')
            dependencies.append(raw.split(b'\0')[0].decode('utf-8'))
        offset += size
    if offset != end or len(minimum) != 1: raise ValueError('Missing or ambiguous macOS minimum version')
    encoded = minimum[0]
    return [{'cpu':cpu, 'architecture':architecture, 'minimumOs':[encoded >> 16, (encoded >> 8) & 255, encoded & 255], 'dependencies':dependencies}]


def audit(root, system=None, architecture=None):
    system = system or platform.system(); architecture = architecture or platform.machine().lower()
    architecture = {'amd64':'x86_64', 'aarch64':'arm64'}.get(architecture, architecture)
    policy = json.loads((ROOT / 'tools/delivery-policy.json').read_text(encoding='utf-8'))
    result = []
    for path in staged_files(root):
        if path.is_symlink(): continue
        with path.open('rb') as stream: magic = stream.read(4)
        record = None
        if magic[:2] == b'MZ':
            if system != 'Windows': raise ValueError('Foreign PE in product')
            record = pe_header(path.read_bytes())
            if record['architecture'] != policy[system]['architecture'] or any(not at_most(record[key],policy[system]['maximumPeVersion']) for key in ('osVersion','subsystemVersion')):
                raise ValueError('Unsupported PE target: ' + str(path))
            record['dependencies'] = pe_imports(path)
        elif magic == b'\x7fELF':
            if system != 'Linux': raise ValueError('Foreign ELF in product')
            record = elf_header(path.read_bytes())
            if record['architecture'] != policy[system]['architecture']: raise ValueError('Unsupported ELF CPU')
            record['neededVersions'] = needed_versions(inspect(['readelf', '--version-info', '-W', path]))
            check_versions(record['neededVersions'], policy[system])
            record['dependencies'], record['rpaths'] = elf_dynamic(inspect(['readelf','-d',path]))
        elif magic in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
            if system != 'Darwin': raise ValueError('Foreign Mach-O in product')
            slices = macho_headers(path.read_bytes())
            if architecture not in {s['architecture'] for s in slices}: raise ValueError('Host architecture missing from delivered Mach-O')
            for item in slices:
                if item['architecture'] not in policy[system]['architectures'] or not at_most(item['minimumOs'],policy[system]['maximumDeploymentTarget']): raise ValueError('Unsupported Mach-O target: '+str(path))
            record = {'slices':slices}
        elif path.suffix.lower() in ('.dll','.exe','.so','.dylib'):
            raise ValueError('Unrecognized native product file: '+str(path))
        if record is not None: result.append({'path':path.relative_to(root).as_posix(),'sha256':digest(path),**record})
    if not result: raise ValueError('No native product files found')
    return {'schemaVersion':1,'ok':True,'system':system,'architecture':architecture,'policy':policy[system], 'files':result,
            'scope':policy['scope'],'minimumOsRuntimeValidated':False,'releaseReady':False,
            'limitations':['PE declarations do not prove individual API availability','runtime-loaded libraries and instruction-set/device behavior require separate validation']}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--root',type=Path,required=True); p.add_argument('--report',type=Path,required=True)
    a=p.parse_args(); value=audit(build_output(a.root)); build_output(a.report).write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'ok':True,'nativeFiles':len(value['files']),'system':value['system'],'minimumOsRuntimeValidated':False}))
