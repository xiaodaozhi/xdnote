"""Seal a product-only Unix runtime, with external probes and file evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import tempfile

import desktop
import deployment_unix as deploy
import user_install
from background_validation import execute, offscreen_environment
from check_third_party_licenses import audit as source_audit
from installer_common import digest
from unix_binary_evidence import code_sections


def inventory(root):
    return [{'path': path.relative_to(root).as_posix(), 'link': os.readlink(path)} if path.is_symlink()
            else {'path': path.relative_to(root).as_posix(), 'bytes': path.stat().st_size, 'sha256': digest(path)}
            for path in deploy.staged_files(root)]


def require_product(root):
    bad = []
    for item in inventory(root):
        name = item['path'].lower()
        if any(token in name for token in (*deploy.FORBIDDEN, 'qt6quick', 'qt6qml', 'qtquick', 'qtqml', 'qt6test', 'qttest.framework', 'offscreen', 'qmltooling', '_probe', '_tests')) or 'qml' in Path(name).parts:
            bad.append(item['path'])
    if bad: raise ValueError('Non-product runtime files: ' + ', '.join(bad))


def locations(root, system):
    if system == 'Darwin':
        contents = root / 'xdnote.app/Contents'
        return contents / 'MacOS', contents / 'PlugIns', contents / 'Frameworks', contents / 'Resources'
    return root / 'bin', root / 'plugins', root / 'lib', root


def material_evidence(root, qt, records, system):
    """Supplier hashes and executable code identity; rewritten data is separate."""
    bindir, plugins, libraries, notices = locations(root, system)
    supplier = {}
    for record in records:
        raw = notices / 'licenses/qt' / record['sbom']
        if digest(raw) != record['sha256'] or digest(qt / 'sbom' / record['sbom']) != record['sha256']:
            raise ValueError('Upstream SBOM changed')
        for item in json.loads(raw.read_text(encoding='utf-8')).get('files', []):
            name = item.get('fileName', '').removeprefix('./')
            if Path(name).is_absolute() or '..' in Path(name).parts: raise ValueError('Unbounded supplier path')
            supplier[name] = {'sbom': record['sbom'], 'spdxId': item['SPDXID'],
                              'licenseOptions': item.get('licenseConcluded', 'NOASSERTION'),
                              'copyright': item.get('copyrightText', 'NOASSERTION'), 'checksums': item.get('checksums', [])}
    result = []
    for item in inventory(root):
        path = root / item['path']; origin = None
        if 'link' in item:
            record = {'kind': 'contained relative link', 'resolved': path.resolve().relative_to(root).as_posix()}
        elif path.is_relative_to(libraries):
            origin = qt / 'lib' / path.relative_to(libraries)
            record = {'kind': 'Qt framework or runtime material'}
        elif path.is_relative_to(plugins):
            origin = qt / 'plugins' / path.relative_to(plugins)
            record = {'kind': 'Qt plugin'}
        elif path.is_relative_to(bindir) and path.name in ('xdnote', 'xdnote-resource-helper'):
            record = {'kind': 'project application or resource helper', 'staticDependencies': 'locked Rust and Scintilla source materials; standard library/runtime review pending'}
        elif path.is_relative_to(notices / 'licenses'):
            relative = path.relative_to(notices / 'licenses').as_posix()
            source = None
            if relative.startswith('project-and-source-dependencies/'):
                source = desktop.ROOT / 'LICENSES' / relative.removeprefix('project-and-source-dependencies/')
            elif relative in ('Apache-2.0.txt', 'THIRD_PARTY_NOTICES.md'):
                source = desktop.ROOT / ('LICENSE' if relative == 'Apache-2.0.txt' else relative)
            elif relative.startswith('qt/') and path.name in {r['sbom'] for r in records}:
                source = qt / 'sbom' / path.name
            elif relative == 'DESIGN-ATTRIBUTION.txt':
                if 'CC BY 4.0' not in path.read_text(encoding='utf-8'): raise ValueError('Design attribution missing')
            elif relative != 'qt/UPSTREAM-NOTICES.txt':
                raise ValueError('Unexpected license material: ' + relative)
            record = {'kind': 'preserved license or attribution material'}
            if source is not None:
                if not source.is_file() or digest(source) != item['sha256']: raise ValueError('License material changed: ' + relative)
                record['sourceSha256'] = digest(source)
        elif path.name in ('qt.conf', 'Info.plist') or '_CodeSignature' in path.parts:
            record = {'kind': 'generated bundle configuration or ad-hoc seal'}
        else:
            raise ValueError('Unclassified product file: ' + item['path'])
        if origin is not None:
            if '_CodeSignature' in path.parts:
                record = {'kind': 'regenerated Qt ad-hoc seal'}
            else:
                if not origin.is_file() or not origin.resolve().is_relative_to(qt):
                    raise ValueError('Missing bounded Qt supplier file: ' + str(origin))
                name = origin.relative_to(qt).as_posix()
                record.update(sdkFile=name, sdkSha256=digest(origin), byteIdenticalToSdk=digest(origin) == item['sha256'])
                if deploy.binary(path, system):
                    before, after = code_sections(origin.read_bytes()), code_sections(path.read_bytes())
                    if before != after: raise ValueError('Deployed Qt executable sections differ: ' + item['path'])
                    if name not in supplier: raise ValueError('Qt binary lacks exact SPDX file evidence: ' + name)
                    record.update(supplier=supplier[name], executableSections=after, executableSectionsMatchSdk=True,
                                  transformations='directory-local rpath and imports; macdeployqt/ad-hoc seal on macOS; no claim of complete file-byte equivalence')
        result.append(dict(item, evidence=record))
    return {'ok': True, 'files': result, 'qtModules': records, 'licenseReviewComplete': False, 'releaseReady': False,
            'pending': ['final compiled-in third-party scope and license options', 'Qt corresponding source and replacement obligations', 'Rust standard library and compiler runtime', 'final release artifact review'],
            'scope': 'actual file hashes, supplier paths and code sections; metadata/signature changes recorded separately; no signing trust or final license determination'}


def relocate_probe(path, qt, libraries, system):
    if system == 'Linux':
        deploy.inspect(['patchelf', '--set-rpath', str(libraries), path])
        return
    imports, paths, _ = deploy.macho_loads(deploy.inspect(['/usr/bin/otool', '-l', path]))
    for entry in paths: deploy.inspect(['/usr/bin/install_name_tool', '-delete_rpath', entry, path])
    deploy.inspect(['/usr/bin/install_name_tool', '-add_rpath', libraries, path])
    for name in imports:
        if name.startswith(str(qt / 'lib') + '/'):
            deploy.inspect(['/usr/bin/install_name_tool', '-change', name, '@rpath/' + name[len(str(qt / 'lib')) + 1:], path])
    deploy.inspect(['/usr/bin/codesign', '--force', '--sign', '-', path])


def smoke(root, build, qt, evidence, system):
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='external-probe-', dir=evidence) as directory:
        return run_probe(root, build, qt, evidence, system, Path(directory))


def run_probe(root, build, qt, evidence, system, harness):
    bindir, plugins, libraries, _ = locations(root, system)
    probe = harness / 'xdnote_deployment_probe'
    shutil.copy2(build / probe.name, probe)
    (harness / 'xdnote-resource-helper').symlink_to(bindir / 'xdnote-resource-helper')
    backend = harness / 'platforms' / ('libqoffscreen.so' if system == 'Linux' else 'libqoffscreen.dylib')
    backend.parent.mkdir()
    shutil.copy2(qt / 'plugins/platforms' / backend.name, backend)
    for path in (probe, backend): relocate_probe(path, qt, libraries, system)
    env = offscreen_environment(deploy.clean_environment(), evidence)
    env.update(QT_PLUGIN_PATH=str(plugins), QT_QPA_PLATFORM_PLUGIN_PATH=str(backend.parent))
    if system == 'Linux': env['LD_LIBRARY_PATH'] = str(libraries)
    records = []
    for theme in ('light', 'dark'):
        output, _ = execute([probe, theme, evidence / (theme + '-standalone.png'), evidence / (theme + '-workspace.png')], env, evidence / (theme + '.log'), 120)
        values = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
        if len(values) != 1: raise RuntimeError('Unexpected external probe output')
        value = values[0]
        if not value.get('ok') or not value.get('editUndoSaveIntegrity') or not value.get('uniqueSessionTransfer') or value.get('nativeImages') != 1:
            raise RuntimeError('Actual product runtime probe failed: ' + json.dumps(value))
        if not Path(value['qtCorePath']).resolve().is_relative_to(root) or Path(value['resourceHelper']).resolve() != (bindir / 'xdnote-resource-helper').resolve():
            raise RuntimeError('Probe used a runtime outside the actual product')
        records.append(value)
    return records


def prepare(build, qt, root):
    system = platform.system(); identity = desktop.verify_build_identity(build, 'default', qt)
    if source_audit(desktop.ROOT): raise ValueError('Source license inputs failed')
    evidence = root.with_name(root.name + '-evidence')
    if any(p.exists() or p.is_symlink() for p in (root, evidence)): raise ValueError('Choose new product and evidence directories')
    root.mkdir(parents=True); evidence.mkdir()
    report = {'ok': False, 'kind': 'product-only Unix developer payload', 'host': {'system': system, 'architecture': platform.machine()},
              'buildIdentity': identity, 'releaseReady': False, 'licenseReviewComplete': False}
    try:
        (deploy.linux_stage if system == 'Linux' else deploy.mac_stage)(build, qt, root, evidence, product=True)
        require_product(root)
        bindir, _, _, notices = locations(root, system)
        imports, names = deploy.unix_closure(root, bindir, system)
        licenses = notices / 'licenses'
        shutil.copytree(desktop.ROOT / 'LICENSES', licenses / 'project-and-source-dependencies')
        shutil.copy2(desktop.ROOT / 'LICENSE', licenses / 'Apache-2.0.txt')
        shutil.copy2(desktop.ROOT / 'THIRD_PARTY_NOTICES.md', licenses / 'THIRD_PARTY_NOTICES.md')
        (licenses / 'DESIGN-ATTRIBUTION.txt').write_text('xdnote 项目及其贡献者；https://github.com/xiaodaozhi/xdnote\n文档与原创设计：CC BY 4.0；https://creativecommons.org/licenses/by/4.0/\n', encoding='utf-8')
        records = deploy.collect_qt_notices(qt, notices, names)
        if system == 'Darwin':
            deploy.inspect(['/usr/bin/codesign', '--force', '--deep', '--sign', '-', root / 'xdnote.app'])
            deploy.inspect(['/usr/bin/codesign', '--verify', '--deep', '--strict', root / 'xdnote.app'])
        materials = material_evidence(root, qt, records, system)
        materials['buildIdentity'] = identity
        (evidence / 'license-evidence.json').write_text(json.dumps(materials, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        sealed = inventory(root)
        probes = smoke(root, build, qt, evidence, system)
        if inventory(root) != sealed: raise RuntimeError('Probe changed sealed product files')
        report.update(imports=imports, missingImports=[], qtSbom=records, files=sealed,
                      probes=probes, signing='ad-hoc integrity only' if system == 'Darwin' else 'none', ok=True)
    except Exception as error:
        report['error'] = str(error); raise
    finally:
        (evidence / 'inventory.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'files': len(report['files']), 'bytes': sum(f.get('bytes', 0) for f in report['files']), 'qtModules': len(records), 'probes': len(report['probes'])}))
    return report


def validate_install(payload, destination, build, qt, metadata):
    system = platform.system(); identity = desktop.verify_build_identity(build, 'default', qt)
    if not metadata.get('ok') or metadata.get('kind') != 'product-only Unix developer payload' or metadata.get('buildIdentity') != identity or metadata.get('files') != inventory(payload):
        raise ValueError('Current sealed product payload required')
    evidence = destination.with_name(destination.name + '-evidence')
    if destination.exists() or evidence.exists(): raise ValueError('Existing install evidence preserved')
    evidence.mkdir(parents=True)
    sentinel = evidence / 'preserved-user-data.json'; sentinel.write_text('{"settings":"own fixture","draft":"preserve"}')
    before = digest(sentinel); steps = []; probes = []; result = {'ok': False, 'buildIdentity': identity, 'realRegistryChanged': False, 'releaseReady': False}
    try:
        steps.append(user_install.install(payload, destination, apply=True, registration=False))
        # The installer validates every copied hash and contained link.
        user_install.checked_installation(destination)
        for phase in ('installed', 'upgraded'):
            if phase == 'upgraded': steps.append(user_install.install(payload, destination, apply=True, registration=False))
            probes.extend(smoke(destination, build, qt, evidence / phase, system))
        bindir, _, _, _ = locations(destination, system)
        application = bindir / 'xdnote'; original = application.read_bytes()
        try:
            application.write_bytes(original + b'guard fixture')
            for operation in (lambda: user_install.install(payload, destination, apply=True), lambda: user_install.uninstall(destination, apply=True)):
                try: operation()
                except ValueError: pass
                else: raise RuntimeError('Modified application was not protected')
            if application.read_bytes() != original + b'guard fixture': raise RuntimeError('Modified file overwritten')
        finally: application.write_bytes(original)
        unknown = destination / '保留文档.md'; unknown.write_text('own unknown document', encoding='utf-8')
        try:
            for operation in (lambda: user_install.install(payload, destination, apply=True), lambda: user_install.uninstall(destination, apply=True)):
                try: operation()
                except ValueError: pass
                else: raise RuntimeError('Unknown file was not protected')
            if unknown.read_text(encoding='utf-8') != 'own unknown document': raise RuntimeError('Unknown document changed')
        finally: unknown.unlink()
        steps.append(user_install.uninstall(destination, apply=True, registration=False))
        if destination.exists() or digest(sentinel) != before: raise RuntimeError('Installation roundtrip changed owned data fixture')
        result.update(ok=True, steps=steps, probes=probes, modifiedFileProtected=True, unknownFileProtected=True,
                      fixtureSha256Before=before, fixtureSha256After=digest(sentinel), payloadFiles=len(metadata['files']))
    except Exception as error:
        result['error'] = str(error); raise
    finally:
        (evidence / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'probes': len(probes), 'modifiedFileProtected': True, 'unknownFileProtected': True}))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'qt-prefix', 'destination'): p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--payload', type=Path); p.add_argument('--audit', type=Path)
    a = p.parse_args()
    if platform.system() not in ('Linux', 'Darwin'): p.error('Requires a Linux or macOS host')
    build, qt, destination = desktop.build_path(a.build_dir), desktop.checked_tool_path(a.qt_prefix, 'Qt SDK', sdk=True), desktop.build_path(a.destination)
    if a.payload:
        if not a.audit: p.error('--payload requires --audit')
        validate_install(desktop.build_path(a.payload), destination, build, qt, json.loads(a.audit.read_text(encoding='utf-8')))
    else: prepare(build, qt, destination)
