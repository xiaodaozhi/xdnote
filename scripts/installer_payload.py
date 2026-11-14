"""Seal a product-only Windows payload from the verified diagnostic deployment."""
import argparse
import json
import os
from pathlib import Path
import shutil

import desktop
from deployment_inventory import dependency_closure, files_under
from installer_common import ROOT, build_output, digest

DIAGNOSTIC_FILES = frozenset(('xdnote_background_probe.exe', 'xdnote_preview_product_probe.exe',
                              'Qt6Test.dll', 'platforms/qoffscreen.dll'))


def inventory(root):
    return [{'path': p.relative_to(root).as_posix(), 'bytes': p.stat().st_size,
             'sha256': digest(p)} for p in files_under(root)]


def verify_payload(root, audit):
    root = Path(root)
    if root.is_symlink() or root.is_junction() or not root.is_dir():
        raise ValueError('Payload must be a real directory')
    if audit.get('ok') is not True or audit.get('files') != inventory(root):
        raise ValueError('Payload no longer matches its sealed file inventory')


def product_qt_records(records, files):
    shipped = {Path(item['path']).name.casefold() for item in files}
    result = []
    for record in records:
        matched = [name for name in record['matchedBinaryNames'] if name.casefold() in shipped]
        if matched:
            result.append(dict(record, matchedBinaryNames=matched))
    return result


def prepare(source, source_audit, destination, toolchain, runtime, build, qt):
    build, qt = build.resolve(), qt.resolve()
    source, toolchain, runtime = (build_output(path) for path in (source, toolchain, runtime))
    desktop.verify_build_identity(build, 'default', qt)
    if source_audit.get('buildIdentity') != json.loads((build / 'build-identity.json').read_text()):
        raise ValueError('Deployment and current build identities differ')
    verify_payload(source, source_audit)
    destination = build_output(destination)
    if destination.exists():
        raise ValueError('Use a new installer payload directory')
    config = json.loads((ROOT / 'scripts/installer-toolchain.json').read_text())['inno_setup']
    crt = json.loads((ROOT / 'scripts/windows-runtime.json').read_text())
    for item in source_audit['files']:
        name = item['path']
        if name in DIAGNOSTIC_FILES:
            continue
        # App-local CRT bytes must correspond to the supplier agreement.
        if name.lower().startswith(('msvcp', 'vcruntime')) and name.lower().endswith('.dll'):
            if crt['files'].get(name.lower()) != item['sha256']:
                raise ValueError('Product CRT does not match the pinned supplier package')
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    notice = destination / 'share/notices'
    (notice / 'Microsoft-VC-runtime').mkdir(parents=True)
    original = runtime / 'Microsoft-VC-runtime-license.rtf'
    if digest(original) != crt['license_sha256']:
        raise ValueError('Original runtime agreement is missing or changed')
    shutil.copyfile(original, notice / 'Microsoft-VC-runtime/license.rtf')
    for item in config['materials']:
        if item['file'] == 'ChineseSimplified.isl':
            continue
        original = toolchain / item['file']
        if digest(original) != item['sha256']:
            raise ValueError('Installer engine notice is missing or changed')
        shutil.copyfile(original, notice / item['file'])
    text = (ROOT / 'apps/desktop/packaging/windows/Windows-runtime-NOTICE.txt').read_bytes()
    (notice / 'Windows-runtime-NOTICE.txt').write_bytes(b'\xef\xbb\xbf' + text)
    (notice / 'Installer-CREDITS.txt').write_text(
        'Installer: Inno Setup 6.7.3; see InnoSetup-license.txt.\n'
        'Uses RemObjects Pascal Script (https://www.remobjects.com/ps); '
        'see InnoSetup-PascalScript-license.md.\n', encoding='utf-8')
    imports, missing = dependency_closure(destination, Path(os.environ['SystemRoot']) / 'System32')
    if missing:
        raise ValueError('Product-only payload has unresolved imports: ' + ', '.join(missing))
    result = dict(source_audit)
    files = inventory(destination)
    result.update(kind='unsigned product-only Windows installer payload', files=files,
                  qtSbom=product_qt_records(source_audit['qtSbom'], files),
                  imports=imports, missingImports=missing, removedDiagnosticFiles=sorted(DIAGNOSTIC_FILES),
                  diagnosticOnly=[], offscreenSmoke=[], offscreenProductSmoke=None)
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for option in ('source', 'audit', 'destination', 'toolchain', 'runtime', 'build-dir', 'qt-prefix', 'report'):
        p.add_argument('--' + option, type=Path, required=True)
    a = p.parse_args()
    result = prepare(a.source, json.loads(a.audit.read_text(encoding='utf-8')), a.destination,
                     a.toolchain, a.runtime, a.build_dir, a.qt_prefix)
    build_output(a.report).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'files': len(result['files']), 'bytes': sum(f['bytes'] for f in result['files']),
                      'missingImports': result['missingImports'], 'releaseReady': False}))
