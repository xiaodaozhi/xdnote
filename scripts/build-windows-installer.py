"""Compile an unsigned, current-user graphical development installer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import desktop
from installer_common import ROOT, build_output, digest
from installer_payload import verify_payload, DIAGNOSTIC_FILES


def payload_manifest(audit):
    entries, seen = [], set()
    for item in sorted(audit['files'], key=lambda f: f['path'].casefold()):
        path = item['path'].replace('/', '\\')
        parts = path.split('\\')
        devices = {'con', 'prn', 'aux', 'nul', 'clock$', *(f'com{i}' for i in range(1, 10)),
                   *(f'lpt{i}' for i in range(1, 10))}
        if (any(not x or x in ('.', '..') or x[-1] in '. ' or x.split('.')[0].casefold() in devices for x in parts)
                or any(ord(c) < 32 or c in '/:*?"<>|' for c in path) or len(path) > 1024
                or path.casefold() in seen or parts[0].casefold() in ('versions', 'install-manifest.txt')
                or parts[0].lower().startswith('unins') or not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
            raise ValueError('Unsafe or reserved installer payload path')
        seen.add(path.casefold())
        entries.append(item['sha256'] + '\t' + path)
    text = 'xdnote-install-files-v1\n' + '\n'.join(entries) + '\n'
    if not entries or len(entries) > 16384 or len(text.encode('utf-8')) > 2097152:
        raise ValueError('Installer manifest exceeds bounded format')
    return text


def render(payload, audit, output, chinese, version):
    verify_payload(payload, audit)
    if not re.fullmatch(r'0\.1\.0-dev\.[0-9a-f]{12}(?:-qa[12])?', version):
        raise ValueError('Use an explicit development version, not an invented release')
    values = {'VERSION': version, 'OUTPUT': str(output.parent.resolve()), 'FILENAME': output.stem,
              'APPICON': str(ROOT / 'design/assets/ui-v2/brand/app-icon.ico'),
              'PAYLOAD': str(payload.resolve()), 'CHINESE': str(chinese.resolve()),
              'MANIFEST': str(output.with_suffix('.files').resolve())}
    text = (ROOT / 'apps/desktop/packaging/windows/installer.iss.in').read_text(encoding='utf-8')
    for key, value in values.items():
        if any(c in value for c in ('"', '\r', '\n', ';', '{', '}')):
            raise ValueError('Unsafe installer substitution')
        text = text.replace('@' + key + '@', value)
    text = text.replace('@CLEANUP_CODE@', (ROOT / 'apps/desktop/packaging/windows/installed-files.iss').read_text(encoding='utf-8'))
    if re.search(r'@[A-Z_]+@', text):
        raise ValueError('Unresolved installer substitution')
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for option in ('root', 'audit', 'toolchain', 'output', 'report'):
        p.add_argument('--' + option, type=Path, required=True)
    p.add_argument('--qa-variant', choices=('1', '2'))
    a = p.parse_args()
    a.toolchain = build_output(a.toolchain)
    a.root = build_output(a.root)
    if a.qa_variant and (os.environ.get('CI') != 'true' or os.environ.get('RUNNER_OS') != 'Windows'):
        p.error('Installer fixtures are restricted to disposable Windows CI')
    output = build_output(a.output)
    if output.suffix.lower() != '.exe' or output.exists():
        p.error('Choose a new .exe output')
    audit = json.loads(a.audit.read_text(encoding='utf-8'))
    if not a.qa_variant:
        if (audit.get('kind') != 'unsigned product-only Windows installer payload'
                or audit.get('buildIdentity', {}).get('sourceSha256') != desktop.source_fingerprint()
                or sorted(audit.get('removedDiagnosticFiles', [])) != sorted(DIAGNOSTIC_FILES)):
            raise ValueError('A current, product-only sealed deployment is required')
    config = json.loads((ROOT / 'scripts/installer-toolchain.json').read_text())['inno_setup']
    crt = json.loads((ROOT / 'scripts/windows-runtime.json').read_text())
    setup = a.toolchain / 'compiler-setup.exe'
    if digest(setup) != config['sha256']:
        raise ValueError('Installer compiler package identity mismatch')
    for item in config['materials']:
        if digest(a.toolchain / item['file']) != item['sha256']:
            raise ValueError('Pinned compiler material identity mismatch')
        if item['file'] != 'ChineseSimplified.isl' and digest(a.root / 'share/notices' / item['file']) != item['sha256']:
            raise ValueError('Payload lacks original installer engine notices')
    if digest(a.root / 'share/notices/Microsoft-VC-runtime/license.rtf') != crt['license_sha256']:
        raise ValueError('Payload lacks the original supplier runtime agreement')
    notice = ROOT / 'apps/desktop/packaging/windows/Windows-runtime-NOTICE.txt'
    if (a.root / 'share/notices/Windows-runtime-NOTICE.txt').read_bytes() != b'\xef\xbb\xbf' + notice.read_bytes():
        raise ValueError('Payload runtime notice changed')
    credits = a.root / 'share/notices/Installer-CREDITS.txt'
    if 'RemObjects Pascal Script (https://www.remobjects.com/ps)' not in credits.read_text(encoding='utf-8'):
        raise ValueError('Pascal Script acknowledgement missing')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip())
    version = '0.1.0-dev.' + revision[:12] + ('-qa' + a.qa_variant if a.qa_variant else '')
    text = render(a.root, audit, output, a.toolchain / 'ChineseSimplified.isl', version)
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = payload_manifest(audit)
    output.with_suffix('.files').write_bytes(manifest.encode('utf-8'))
    output.with_suffix('.iss').write_text(text, encoding='utf-8-sig')
    subprocess.run([str(a.toolchain / 'compiler/ISCC.exe'), str(output.with_suffix('.iss'))], check=True)
    verify_payload(a.root, audit)
    result = {'schemaVersion': 1, 'ok': True, 'version': version, 'revision': revision,
              'dirty': dirty, 'buildIdentity': audit.get('buildIdentity'), 'qaFixture': bool(a.qa_variant),
              'file': output.name, 'sha256': digest(output), 'signed': False, 'releaseReady': False,
              'compiler': config['version'], 'compilerPackageSha256': config['sha256'],
              'payloadManifestSha256': hashlib.sha256(manifest.encode('utf-8')).hexdigest(),
              'runtimeAgreementSha256': crt['license_sha256'], 'scope': 'current user'}
    build_output(a.report).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
