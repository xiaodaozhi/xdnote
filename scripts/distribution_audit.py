"""Bind actual Windows payload files to license materials, without legal conclusions."""
import argparse
import hashlib
import json
import struct
from pathlib import Path, PurePosixPath

from check_third_party_licenses import audit as source_audit
from installer_common import ROOT, build_output, digest
from installer_payload import verify_payload, product_qt_records


def checked_bytes(path, expected):
    if not path.is_file() or digest(path) != expected:
        raise ValueError('Material or supplier byte mismatch: ' + str(path))


def unsigned_pe_match(data, expected):
    """Compare a reconstructed pre-certificate file; never alter the DLL.

    This only compares bytes. It does not validate certificate trust or license
    compliance. Original vendor checksums and delivered hashes remain intact.
    """
    try:
        if data[:2] != b'MZ': return False
        pe, = struct.unpack_from('<I', data, 0x3c)
        if data[pe:pe + 4] != b'PE\0\0': return False
        optional = pe + 24
        if struct.unpack_from('<H', data, optional)[0] != 0x20b: return False
        security = optional + 112 + 4 * 8
        offset, size = struct.unpack_from('<II', data, security)
        if not size or offset < security + 8 or offset + size != len(data): return False
        unsigned = bytearray(data[:offset])
        unsigned[optional + 64:optional + 68] = bytes(4)
        unsigned[security:security + 8] = bytes(8)
        for padding in range(8):
            if padding and any(unsigned[-padding:]): continue
            candidate = unsigned[:-padding] if padding else unsigned
            if hashlib.sha1(candidate).hexdigest() == expected: return True
    except (struct.error, IndexError):
        return False
    return False


def qt_files(root, qt, records, shipped):
    result = {}
    for record in records:
        sbom = root / 'licenses/qt' / record['sbom']
        checked_bytes(sbom, record['sha256'])
        checked_bytes(qt / 'sbom' / record['sbom'], record['sha256'])
        value = json.loads(sbom.read_text(encoding='utf-8'))
        for item in value.get('files', []):
            relative = PurePosixPath(item['fileName'].removeprefix('./'))
            if not relative.parts or relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unbounded supplier file path')
            if relative.parts[0] not in ('bin', 'plugins'):
                continue
            target = relative.as_posix()[4:] if relative.parts[0] == 'bin' else relative.as_posix()[8:]
            if target not in shipped or not target.lower().endswith('.dll'):
                continue
            source = qt / relative
            checked_bytes(source, shipped[target]['sha256'])
            checksums = {entry['algorithm']: entry['checksumValue'] for entry in item.get('checksums', [])}
            data = (root / target).read_bytes()
            actual_sha1 = hashlib.sha1(data).hexdigest()
            if target in result:
                raise ValueError('Ambiguous Qt file evidence: ' + target)
            result[target] = {'kind': 'Qt runtime or plugin', 'sdkFile': relative.as_posix(),
                'sdkSha256Matches': True, 'sbom': record['sbom'], 'spdxId': item['SPDXID'],
                'licenseOptions': item.get('licenseConcluded', 'NOASSERTION'),
                'copyright': item.get('copyrightText', 'NOASSERTION'),
                'upstreamFileChecksums': checksums, 'deliveredSha1': actual_sha1,
                'upstreamSha1Matches': actual_sha1 == checksums.get('SHA1') if 'SHA1' in checksums else None,
                'unsignedPeSha1Matches': unsigned_pe_match(data, checksums.get('SHA1'))}
    return result


def audit(root, metadata, qt):
    verify_payload(root, metadata)
    problems = source_audit(ROOT)
    if problems:
        raise ValueError('Source materials failed: ' + '; '.join(problems))
    shipped = {item['path']: item for item in metadata['files']}
    source_manifest = json.loads((ROOT / 'LICENSES/third-party/manifest.json').read_text())
    required = {'licenses/Apache-2.0.txt', 'licenses/THIRD_PARTY_NOTICES.md', 'licenses/DESIGN-ATTRIBUTION.txt',
                'licenses/project-and-source-dependencies/CC-BY-4.0.txt',
                'licenses/project-and-source-dependencies/third-party/manifest.json',
                'licenses/project-and-source-dependencies/third-party/Qt-LGPL-3.0-only.txt',
                'licenses/project-and-source-dependencies/third-party/Qt-GPL-3.0-only.txt',
                'share/notices/Microsoft-VC-runtime/license.rtf', 'share/notices/Windows-runtime-NOTICE.txt',
                'share/notices/InnoSetup-license.txt', 'share/notices/InnoSetup-PascalScript-license.md',
                'share/notices/Installer-CREDITS.txt'}
    required.update('licenses/project-and-source-dependencies/' + item['licenseFile'].removeprefix('LICENSES/')
                    for item in [*source_manifest['rust'], source_manifest['scintilla']])
    if required - set(shipped):
        raise ValueError('Required distribution material missing: ' + ', '.join(sorted(required - set(shipped))))
    records = product_qt_records(metadata['qtSbom'], metadata['files'])
    evidence = qt_files(root, qt, records, shipped)
    crt = json.loads((ROOT / 'scripts/windows-runtime.json').read_text())
    compiler = json.loads((ROOT / 'scripts/installer-toolchain.json').read_text())['inno_setup']
    manifest = source_manifest
    materials = {item['file']: item for item in compiler['materials']}
    for name in shipped:
        file = root / name
        if name in evidence:
            continue
        record = None
        if name in crt['files']:
            checked_bytes(file, crt['files'][name])
            record = {'kind': 'Microsoft VC runtime', 'supplierVersion': crt['version'],
                      'agreement': 'share/notices/Microsoft-VC-runtime/license.rtf'}
        elif name == 'D3Dcompiler_47.dll':
            raise ValueError('Host system graphics DLL must not be bundled')
        elif name == 'xdnote.exe':
            record = {'kind': 'project application with static dependencies', 'projectCodeLicense': 'Apache-2.0',
                      'rustPackages': [{'name': item['name'], 'version': item['version'], 'license': item['selectedLicense']}
                                       for item in manifest['rust']], 'scintillaVersion': manifest['scintilla']['version'],
                      'compilerRuntimeAndStandardLibraryReview': 'pending'}
        elif name.startswith('licenses/project-and-source-dependencies/'):
            source = ROOT / 'LICENSES' / name.removeprefix('licenses/project-and-source-dependencies/')
            checked_bytes(file, digest(source))
            record = {'kind': 'preserved source dependency license material'}
        elif name in ('licenses/Apache-2.0.txt', 'licenses/THIRD_PARTY_NOTICES.md'):
            source = ROOT / ('LICENSE' if name.endswith('Apache-2.0.txt') else 'THIRD_PARTY_NOTICES.md')
            checked_bytes(file, digest(source)); record = {'kind': 'preserved project notice'}
        elif name == 'licenses/DESIGN-ATTRIBUTION.txt':
            if 'CC BY 4.0' not in file.read_text(encoding='utf-8'):
                raise ValueError('Design attribution missing')
            record = {'kind': 'original design attribution', 'license': 'CC BY 4.0'}
        elif name.startswith('licenses/qt/'):
            if name.endswith('.spdx.json') and Path(name).name not in {r['sbom'] for r in records}:
                raise ValueError('Unmatched Qt module material')
            record = {'kind': 'unmodified upstream SBOM or collected module-wide notices',
                      'scope': 'may contain build tools and components absent from the delivered binaries'}
        elif name == 'share/notices/Microsoft-VC-runtime/license.rtf':
            checked_bytes(file, crt['license_sha256']); record = {'kind': 'original Microsoft runtime agreement'}
        elif name.startswith('share/notices/') and Path(name).name in materials:
            checked_bytes(file, materials[Path(name).name]['sha256']); record = {'kind': 'original installer engine license'}
        elif name == 'share/notices/Windows-runtime-NOTICE.txt':
            expected = b'\xef\xbb\xbf' + (ROOT / 'apps/desktop/packaging/windows/Windows-runtime-NOTICE.txt').read_bytes()
            if file.read_bytes() != expected: raise ValueError('Runtime scope notice changed')
            record = {'kind': 'project runtime scope notice'}
        elif name == 'share/notices/Installer-CREDITS.txt':
            if 'RemObjects Pascal Script (https://www.remobjects.com/ps)' not in file.read_text(encoding='utf-8'):
                raise ValueError('Installer acknowledgement missing')
            record = {'kind': 'installer acknowledgement'}
        elif name == 'qt.conf':
            record = {'kind': 'generated project runtime configuration'}
        if record is None:
            raise ValueError('Unclassified delivered file: ' + name)
        evidence[name] = record
    checksums = [name for name, item in evidence.items() if item.get('upstreamSha1Matches') is False]
    unexplained = [name for name in checksums if not evidence[name]['unsignedPeSha1Matches']]
    unresolved = [{'module': item['sbom'], 'packages': item['packagesWithUnresolvedLicenseFields'],
                   'scope': 'full upstream module; not a list of proven delivered third-party components'} for item in records]
    return {'schemaVersion': 1, 'kind': 'actual payload license evidence; not a legal-compliance determination',
            'ok': True, 'buildIdentity': metadata['buildIdentity'], 'files': [dict(shipped[name], evidence=evidence[name]) for name in sorted(shipped)],
            'qtModules': records, 'qtUpstreamUnresolvedFields': unresolved, 'qtUpstreamFileChecksumDifferences': checksums,
            'qtUnexplainedFileChecksumDifferences': unexplained,
            'pending': ['review Qt corresponding sources, replacement obligations and actual compiled-in components',
                        'review any unexplained upstream file-checksum differences; PE comparison does not validate signing trust',
                        'Rust standard library and compiler runtime materials',
                        'bind final platform delivery artifacts and complete release review'],
            'systemGraphicsPolicy': 'Windows 10+ supplies D3Dcompiler_47.dll; host system DLLs are not distributed',
            'licenseReviewComplete': False, 'releaseReady': False}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'audit', 'qt-prefix', 'report'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    result = audit(build_output(a.root), json.loads(a.audit.read_text(encoding='utf-8')), build_output(a.qt_prefix))
    build_output(a.report).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'files': len(result['files']), 'qtFiles': sum(x['evidence']['kind'] == 'Qt runtime or plugin' for x in result['files']),
                      'qtModules': len(result['qtModules']), 'upstreamChecksumDifferences': len(result['qtUpstreamFileChecksumDifferences']),
                      'licenseReviewComplete': False}))
