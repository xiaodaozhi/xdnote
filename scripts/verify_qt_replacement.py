"""Load source-modified Qt Core in an owned Windows/Linux/macOS installation.

Only build copies change; the sealed product, source input and fixed SDK remain
intact. The probe performs no registry or default-application operation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import desktop
import deployment_unix as deploy
import product_deployment_unix as product
import user_install
from background_validation import execute, offscreen_environment
from installer_common import build_output, digest
from unix_binary_evidence import code_sections

PROOF = 'xdnote-qt-source-rebuild-6.8.2'


def pe_code_sections(data):
    def read(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data): raise ValueError('Truncated PE code evidence')
        return struct.unpack_from(fmt, data, offset)
    if data[:2] != b'MZ': raise ValueError('Not a PE image')
    header, = read('<I', 0x3c)
    if data[header:header + 4] != b'PE\0\0' or read('<H', header + 4)[0] != 0x8664: raise ValueError('PE64 x86_64 required')
    count, = read('<H', header + 6); optional, = read('<H', header + 20)
    if read('<H', header + 24)[0] != 0x20b: raise ValueError('PE64 optional header required')
    result = {}
    for index in range(count):
        offset = header + 24 + optional + index * 40
        name, _, _, length, start, _, _, _, _, flags = read('<8sIIIIIIHHI', offset)
        if not flags & 0x20000000 or not length: continue
        if start + length > len(data): raise ValueError('PE executable section escapes image')
        result[str(index) + ':' + name.rstrip(b'\0').decode('ascii')] = {'bytes': length, 'sha256': hashlib.sha256(data[start:start + length]).hexdigest()}
    if not result: raise ValueError('No PE executable sections')
    return result


def windows_smoke(installed, build, qt, output):
    # Diagnostics enter only this owned copy and are removed before maintenance
    # checks, so the changed Core is the reason for refusal.
    probe = installed / 'xdnote_deployment_probe.exe'; backend = installed / 'platforms/qoffscreen.dll'
    if probe.exists() or backend.exists(): raise ValueError('Unexpected diagnostic in product')
    shutil.copy2(build / probe.name, probe); shutil.copy2(qt / 'plugins/platforms/qoffscreen.dll', backend)
    env = offscreen_environment(desktop.environment(None), output)
    env.update(PATH=str(installed) + os.pathsep + env.get('PATH', ''), QT_PLUGIN_PATH=str(installed), QT_QPA_PLATFORM_PLUGIN_PATH=str(backend.parent),
               XDNOTE_EXPECT_QT_SOURCE_PROOF=PROOF, APPDATA=str(output / 'appdata'), LOCALAPPDATA=str(output / 'localappdata'))
    records = []
    try:
        for theme in ['light', 'dark']:
            text, _ = execute([probe, theme, output / (theme + '-standalone.png'), output / (theme + '-workspace.png')], env, output / (theme + '.log'), 120)
            values = [json.loads(line) for line in text.splitlines() if line.startswith('{"')]
            if len(values) != 1: raise ValueError('Unexpected product probe output')
            value = values[0]
            if not value.get('ok') or not value.get('editUndoSaveIntegrity') or not value.get('uniqueSessionTransfer') or value.get('nativeImages') != 1: raise ValueError('Source replacement probe failed: ' + json.dumps(value))
            if Path(value['qtCorePath']).resolve() != (installed / 'Qt6Core.dll').resolve(): raise ValueError('Runtime escaped owned installation')
            # Windows reads images through native handles in-process; the
            # resource helper is deliberately a Unix-only product target.
            if value.get('resourceHelper'): raise ValueError('Unexpected Windows resource helper')
            records.append(value)
    finally: probe.unlink(); backend.unlink()
    return records


def verify(root, audit, build, qt, replacement, output):
    system = platform.system(); identity = desktop.verify_build_identity(build, 'default', qt)
    if audit.get('files') != product.inventory(root) or audit.get('buildIdentity') != identity: raise ValueError('Current sealed product identity required')
    if output.exists(): raise ValueError('Preserve existing replacement evidence')
    output.mkdir(parents=True); installed = output / 'installed'
    user_install.install(root, installed, apply=True, registration=False)
    relative, sdk_relative = {
        'Darwin': ('xdnote.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore', 'lib/QtCore.framework/Versions/A/QtCore'),
        'Linux': ('lib/libQt6Core.so.6.8.2', 'lib/libQt6Core.so.6.8.2'),
        'Windows': ('Qt6Core.dll', 'bin/Qt6Core.dll')}[system]
    core = installed / relative; sdk_core = qt / sdk_relative
    sdk_hash, original, input_hash = digest(sdk_core), digest(core), digest(replacement)
    sections = pe_code_sections if system == 'Windows' else code_sections
    before, after = sections(core.read_bytes()), sections(replacement.read_bytes())
    if before == after: raise ValueError('Replacement must contain actual code changes')
    shutil.copy2(replacement, core)
    if system == 'Darwin':
        if '_xdnoteQtSourceProof' not in deploy.inspect(['/usr/bin/nm', '-g', replacement]): raise ValueError('Source proof export missing')
        _, rpaths, _ = deploy.macho_loads(deploy.inspect(['/usr/bin/otool', '-l', core]))
        for path in rpaths: deploy.inspect(['/usr/bin/install_name_tool', '-delete_rpath', path, core])
        deploy.inspect(['/usr/bin/install_name_tool', '-add_rpath', '@loader_path/../../..', core])
        deploy.inspect(['/usr/bin/codesign', '--force', '--sign', '-', installed / 'xdnote.app/Contents/Frameworks/QtCore.framework'])
        deploy.inspect(['/usr/bin/codesign', '--force', '--sign', '-', installed / 'xdnote.app'])
        deploy.inspect(['/usr/bin/codesign', '--verify', '--deep', '--strict', installed / 'xdnote.app'])
    elif system == 'Linux': deploy.inspect(['patchelf', '--set-rpath', '$ORIGIN', core])
    os.environ.update(XDNOTE_EXPECT_QT_SOURCE_PROOF=PROOF, XDG_CONFIG_HOME=str(output / 'config'), XDG_DATA_HOME=str(output / 'data'), XDG_CACHE_HOME=str(output / 'cache'))
    if system == 'Darwin': os.environ['CFFIXED_USER_HOME'] = str(output / 'home')
    probes = windows_smoke(installed, build, qt, output) if system == 'Windows' else product.smoke(installed, build, qt, output, system)
    if any(r.get('qtSourceProof') != PROOF for r in probes): raise ValueError('Actual source-modified Core was not loaded')
    protections = []
    for name, operation in [('upgrade', lambda: user_install.install(root, installed, apply=True, registration=False)), ('uninstall', lambda: user_install.uninstall(installed, apply=True, registration=False))]:
        try: operation()
        except ValueError: protections.append(name)
        else: raise ValueError('Modified user library was overwritten or removed')
    if product.inventory(root) != audit['files'] or digest(replacement) != input_hash or digest(sdk_core) != sdk_hash: raise ValueError('Original SDK or input changed')
    result = {'ok': True, 'buildIdentity': identity, 'replacementInputSha256': input_hash, 'originalQtCoreSha256': original, 'actualQtCoreSha256': digest(core),
              'originalCodeSections': before, 'replacementCodeSections': after, 'probes': probes, 'modifiedLibraryProtected': protections,
              'fixedSdkChanged': False, 'fixedSdkQtCoreSha256': sdk_hash, 'licenseReviewComplete': False, 'releaseReady': False,
              'scope': 'source-modified Qt Core loaded in an owned developer installation with real application services; not all Qt modules or final corresponding-source obligation acceptance'}
    (output / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['payload', 'audit', 'build-dir', 'qt-prefix', 'replacement', 'output']: p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    result = verify(build_output(a.payload), json.loads(a.audit.read_bytes()), desktop.build_path(a.build_dir),
                    desktop.checked_tool_path(a.qt_prefix, 'Qt SDK', sdk=True), build_output(a.replacement), build_output(a.output))
    print(json.dumps({'ok': result['ok'], 'modifiedLibraryProtected': result['modifiedLibraryProtected']}))
