"""Explicitly prepare the paired QtGui and Cocoa accessibility replacements on macOS."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

import desktop
from installer_common import ROOT, build_output, digest
from qt_network_backports import download_source, features, headers, sdk_file
import qt_source_access


POLICY = ROOT / 'tools/qt-cocoa-backports.json'
RECEIPT = 'xdnote-cocoa-backports.json'
GUI = 'lib/QtGui.framework/Versions/A/QtGui'
PLUGIN = 'plugins/platforms/libqcocoa.dylib'
SEAL = 'lib/QtGui.framework/Versions/A/_CodeSignature/CodeResources'
MODULES = {GUI: '@loader_path/../../..', PLUGIN: '@loader_path/../../lib'}
GUI_FEATURES = ('qtgui-config.h', 'qtgui-config_p.h')


def config():
    value = json.loads(POLICY.read_bytes())
    qt = desktop.DEPS['qt']
    if (value['schemaVersion'] != 1 or value['qtVersion'] != qt['version']
            or value['variant'] != qt['cocoaVariant'] or value['sourceCodeInRelease'] is not False):
        raise ValueError('Qt Cocoa source policy differs')
    for item in value['patches']:
        relative = Path(item['file'])
        if relative.parent != Path('tools/qt-cocoa-patches') or digest(ROOT / relative) != item['sha256']:
            raise ValueError('Pinned Qt Cocoa patch changed')
    return value


def qtbase_source():
    return next(row for row in qt_source_access.config()['modules'] if row['module'] == 'qtbase')


def core_config(qt):
    core = headers(qt, 'Core')
    return [core / 'qconfig.h', core / '6.8.2/QtCore/private/qconfig_p.h',
            core / '6.8.2/QtCore/private/qtcore-config_p.h']


def current_inputs(qt):
    gui = headers(qt, 'Gui')
    return {
        'configurationSha256': digest(POLICY),
        'producerSha256': digest(Path(__file__)),
        'compilerCompatibilitySha256': digest(ROOT / 'cmake/QtCompilerCompatibility.h'),
        'coreConfiguration': {p.relative_to(qt).as_posix(): digest(p) for p in core_config(qt)},
        'guiFeatures': features([gui / GUI_FEATURES[0], gui / '6.8.2/QtGui/private' / GUI_FEATURES[1]]),
    }


def public_from_receipt(record):
    return {key: value for key, value in record.items()
            if key not in {'coreConfiguration', 'originalSeal', 'originalBackups'}}


def verify(qt):
    path = sdk_file(qt, RECEIPT)
    if not path.is_file():
        raise ValueError('Prepare the pinned Qt Cocoa variant with scripts/prepare_qt.py first')
    record = json.loads(path.read_bytes())
    value = config()
    if (record.get('schemaVersion') != 1 or record.get('variant') != value['variant']
            or record.get('source') != qtbase_source()
            or record.get('patches') != value['patches']
            or record.get('sourceCodeInRelease') is not False
            or record.get('system') != 'Darwin' or record.get('architecture') != 'arm64'
            or any(record.get(k) != v for k, v in current_inputs(qt).items())):
        raise ValueError('Qt Cocoa preparation inputs changed')
    verify_installed(qt, record)
    return record


def verify_installed(qt, record):
    if set(record['files']) != set(MODULES) or set(record['originalBackups']) != set(MODULES):
        raise ValueError('Qt Cocoa variant file set differs')
    for name, row in record['files'].items():
        if digest(sdk_file(qt, name)) != row['sha256']:
            raise ValueError('Prepared Qt Cocoa binary changed: ' + name)
        check_backup(record['originalBackups'][name], row['originalSdkSha256'])
    if digest(sdk_file(qt, SEAL)) != record['installedSealSha256']:
        raise ValueError('Prepared QtGui seal changed')
    seal = record['originalSeal']
    if seal:
        check_backup(seal['path'], seal['sha256'])


def check_backup(path, expected):
    original = Path(path)
    checked = desktop.checked_tool_path(original, 'Original Qt Cocoa backup', sdk=True)
    if original.is_symlink() or digest(checked) != expected:
        raise ValueError('Original Qt Cocoa backup changed')
    return checked


def public_record(qt):
    return public_from_receipt(verify(qt))


def inspect(args):
    return subprocess.check_output([str(arg) for arg in args], text=True).splitlines()


def imports(binary):
    return {line.strip().split(' (', 1)[0]
            for line in inspect(['/usr/bin/otool', '-arch', 'arm64', '-L', binary])[1:]}


def verify_binary_compatibility(original, candidate, allow_no_agl):
    def symbols(path):
        return set(inspect(['/usr/bin/nm', '-arch', 'arm64', '-gUj', path]))
    declarations = symbols(original)
    if not declarations or symbols(candidate) != declarations:
        raise ValueError('Rebuilt Qt Cocoa public/exported symbols differ: ' + candidate.name)
    source_imports = imports(original)
    candidate_imports = imports(candidate)
    # The two binaries keep their install IDs; a plugin's own LC_ID_DYLIB
    # may use a different path spelling in the supplier installer.
    source_imports = {value for value in source_imports if not value.endswith('/' + original.name)}
    candidate_imports = {value for value in candidate_imports if not value.endswith('/' + candidate.name)}
    removed = source_imports - candidate_imports
    expected = {'@rpath/QtDBus.framework/Versions/A/QtDBus'}
    if allow_no_agl:
        expected.add('/System/Library/Frameworks/AGL.framework/Versions/A/AGL')
    if candidate_imports - source_imports or removed - expected:
        raise ValueError('Unexpected Qt Cocoa dependency changes: ' + candidate.name)
    build_version = inspect(['/usr/bin/vtool', '-show-build', candidate])
    if not any(line.split() == ['minos', '14.0'] for line in build_version):
        raise ValueError('Derived Qt Cocoa floor differs from the product')
    return {'exportedSymbolsArm64': len(declarations), 'removedImports': sorted(removed)}


def prepare_source(output, source, archive, allow_download, value, qt):
    if archive is None:
        archive = output / source['archive']
    archive = build_output(archive)
    if not archive.is_file():
        if not allow_download:
            raise ValueError('Provide the pinned Qt Base archive or explicitly use --download')
        download_source(archive, source)
    if digest(archive) != source['sha256']:
        raise ValueError('Pinned Qt Base archive differs')
    source_parent = output / 'source'
    source_parent.mkdir()
    with tarfile.open(archive, 'r:xz') as stream:
        stream.extractall(source_parent, filter='data')
    checkout = source_parent / 'qtbase-everywhere-src-6.8.2'

    env = desktop.environment(None)
    for name in tuple(env):
        if (name.upper().startswith(('QT_', 'QML_', 'DYLD_', 'LD_'))
                or name in {'CMAKE_PREFIX_PATH', 'CMAKE_MODULE_PATH', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS',
                            'QTDIR', 'PKG_CONFIG_PATH', 'LIBRARY_PATH', 'C_INCLUDE_PATH',
                            'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH', 'CC', 'CXX', 'SDKROOT'}):
            env.pop(name)
    commands = []

    def run(args, name, timeout=1800):
        args = list(map(str, args))
        commands.append(args)
        with (output / name).open('wb') as log:
            subprocess.run(args, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=timeout)

    for number, patch in enumerate(value['patches'], 1):
        args = ['git', 'apply', '--directory', checkout.relative_to(ROOT), ROOT / patch['file']]
        run(args[:2] + ['--check'] + args[2:], f'patch-{number}-check.log')
        run(args, f'patch-{number}.log')

    sdk = Path(inspect(['/usr/bin/xcrun', '--sdk', 'macosx', '--show-sdk-path'])[0])
    agl = sdk / 'System/Library/Frameworks/AGL.framework'
    without_agl = not any(agl.glob('**/AGL.tbd'))
    compatibility_changes = {}
    if without_agl:
        opengl = checkout / 'cmake/FindWrapOpenGL.cmake'
        original = opengl.read_text()
        redundant = 'target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE ${__opengl_agl_fw_path})'
        if original.count(redundant) != 1:
            raise ValueError('Unexpected Qt AGL configuration')
        opengl.write_text(original.replace(redundant, '# AGL link stub is unavailable in the selected SDK.'))
        compatibility_changes[opengl.relative_to(checkout).as_posix()] = digest(opengl)
    compatibility = ROOT / 'cmake/QtCompilerCompatibility.h'
    core_options = features(core_config(qt))
    cmake = desktop.prepared_tool('cmake', env)
    ninja = desktop.prepared_tool('ninja', env)
    directory = output / 'build'
    args = [
        cmake, '-S', checkout, '-B', directory, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MAKE_PROGRAM=' + ninja,
        '-DCMAKE_INSTALL_PREFIX=' + str(output / 'unused-install'),
        '-DCMAKE_OSX_SYSROOT=' + str(sdk), '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0',
        '-DCMAKE_CXX_COMPILER=/usr/bin/clang++', '-DCMAKE_C_COMPILER=/usr/bin/clang',
        '-DCMAKE_CXX_FLAGS=-include "' + str(compatibility) + '"',
        '-DCMAKE_OBJCXX_FLAGS=-include "' + str(compatibility) + '"',
        '-DBUILD_SHARED_LIBS=ON', '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF',
        '-DFEATURE_gui=ON', '-DFEATURE_network=ON', '-DFEATURE_widgets=OFF',
        '-DFEATURE_sql=OFF', '-DFEATURE_dbus=OFF', '-DFEATURE_system_pcre2=OFF',
        '-DFEATURE_system_libb2=OFF', '-DFEATURE_system_doubleconversion=OFF', '-DFEATURE_openssl=OFF',
    ]
    for option in ('system_zlib', 'zstd', 'icu'):
        args.append('-DFEATURE_' + option + '=' + ('ON' if core_options[option] == '1' else 'OFF'))
    if subprocess.run(['/usr/bin/xcodebuild', '-version'], capture_output=True).returncode:
        args.append('-DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON')
    run(args, 'configure.log')
    gui_features = features([directory / 'src/gui' / name for name in GUI_FEATURES])
    if gui_features != current_inputs(qt)['guiFeatures']:
        raise ValueError('Rebuilt QtGui features differ from the fixed SDK')
    run([cmake, '--build', directory, '--target', 'Gui', 'QCocoaIntegrationPlugin',
         '--parallel', '4'], 'build.log', 3600)
    return commands, compatibility_changes, without_agl


def atomic_copy(source, destination):
    with tempfile.NamedTemporaryFile(dir=destination.parent, prefix='.xdnote-cocoa-', delete=False) as stream:
        temporary = Path(stream.name)
    try:
        shutil.copy2(source, temporary)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def restore_files(qt, backups, seal):
    for name, original in backups.items():
        atomic_copy(Path(original), sdk_file(qt, name))
    signature = sdk_file(qt, SEAL)
    if seal:
        atomic_copy(Path(seal['path']), signature)
    else:
        signature.unlink(missing_ok=True)


def restore(qt):
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True)
    lock = qt / '.xdnote-cocoa-prepare-lock'
    lock.mkdir()
    try:
        path = sdk_file(qt, RECEIPT)
        record = json.loads(path.read_bytes())
        verify_installed(qt, record)
        with tempfile.TemporaryDirectory(prefix='cocoa-restore-', dir=qt) as scratch:
            rollback = {}
            for name in MODULES:
                copy = Path(scratch) / Path(name).name
                shutil.copy2(sdk_file(qt, name), copy)
                rollback[name] = copy
            seal_path = sdk_file(qt, SEAL)
            sealed = None
            if seal_path.is_file():
                copy = Path(scratch) / 'CodeResources'
                shutil.copy2(seal_path, copy)
                sealed = {'path': copy}
            try:
                restore_files(qt, record['originalBackups'], record['originalSeal'])
                path.unlink()
            except BaseException:
                restore_files(qt, rollback, sealed)
                raise
    finally:
        lock.rmdir()


def install(qt, output, receipt):
    lock = qt / '.xdnote-cocoa-prepare-lock'
    lock.mkdir()
    backups, seal = {}, None
    originals = {}
    receipt_path = sdk_file(qt, RECEIPT)
    try:
        if receipt_path.exists():
            return verify(qt)
        for name in MODULES:
            original = sdk_file(qt, name)
            backup = output / 'original-sdk' / name
            backup.parent.mkdir(parents=True, exist_ok=True)
            if not backup.exists():
                atomic_copy(original, backup)
            originals[name] = digest(original)
            check_backup(backup, originals[name])
            backups[name] = str(backup)
        code_resources = sdk_file(qt, SEAL)
        if code_resources.is_file():
            backup = output / 'original-sdk' / SEAL
            backup.parent.mkdir(parents=True, exist_ok=True)
            atomic_copy(code_resources, backup)
            seal = {'path': str(backup), 'sha256': digest(backup)}
        try:
            for name, rpath in MODULES.items():
                original = sdk_file(qt, name)
                atomic_copy(output / 'build' / name, original)
                subprocess.run(['/usr/bin/install_name_tool', '-rpath', str(output / 'build/lib'), rpath,
                                str(original)], check=True, capture_output=True)
            for path in (qt / 'lib/QtGui.framework', qt / PLUGIN):
                subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)], check=True, capture_output=True)
                subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)], check=True, capture_output=True)
            record = {
                'schemaVersion': 1, **receipt['inputs'], 'variant': receipt['variant'],
                'source': receipt['source'], 'patches': receipt['patches'],
                'system': 'Darwin', 'architecture': 'arm64',
                'sourceBuildReceiptSha256': digest(output / 'build-receipt.json'),
                'compatibility': receipt['compatibility'],
                'files': {name: {'sha256': digest(sdk_file(qt, name)), 'originalSdkSha256': originals[name]}
                          for name in MODULES},
                'installedSealSha256': digest(code_resources),
                'originalBackups': backups, 'originalSeal': seal,
                'sourceCodeInRelease': False, 'licenseReviewComplete': False, 'releaseReady': False,
            }
            with tempfile.NamedTemporaryFile(dir=qt, prefix='.xdnote-cocoa-', delete=False) as stream:
                path = Path(stream.name)
            try:
                path.write_text(json.dumps(record, indent=2) + '\n')
                path.replace(receipt_path)
            finally:
                path.unlink(missing_ok=True)
            return verify(qt)
        except BaseException:
            restore_files(qt, backups, seal)
            receipt_path.unlink(missing_ok=True)
            raise
    finally:
        lock.rmdir()


def build(qt, output, archive=None, allow_download=False):
    if platform.system() != 'Darwin':
        raise ValueError('Qt Cocoa preparation is macOS-only')
    desktop.check_host('Darwin', platform.machine(), platform.mac_ver()[0])
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True)
    output = build_output(output)
    if sdk_file(qt, RECEIPT).exists():
        return verify(qt)
    value = config()
    source = qtbase_source()
    inputs = current_inputs(qt)
    receipt_path = output / 'build-receipt.json'
    if output.exists():
        if not receipt_path.is_file():
            raise ValueError('Preserve the incomplete Qt Cocoa build; choose a new output')
        receipt = json.loads(receipt_path.read_bytes())
        if (receipt['inputs'] != inputs or receipt['variant'] != value['variant']
                or receipt['source'] != source or receipt['patches'] != value['patches']
                or set(receipt['files']) != set(MODULES)):
            raise ValueError('Cached Qt Cocoa build inputs differ')
        for name, expected in receipt['files'].items():
            if digest(output / 'build' / name) != expected:
                raise ValueError('Cached Qt Cocoa binary changed')
    else:
        output.mkdir(parents=True)
        commands, adjustments, without_agl = prepare_source(output, source, archive, allow_download, value, qt)
        compatibility = {name: verify_binary_compatibility(sdk_file(qt, name), output / 'build' / name, without_agl)
                         for name in MODULES}
        receipt = {'inputs': inputs, 'source': source, 'variant': value['variant'], 'patches': value['patches'],
                   'commands': commands, 'sourceAdjustments': adjustments, 'compatibility': compatibility,
                   'files': {name: digest(output / 'build' / name) for name in MODULES}}
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    return install(qt, output, receipt)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--download', action='store_true')
    parser.add_argument('--restore-original', action='store_true')
    args = parser.parse_args()
    if args.restore_original:
        restore(args.qt_prefix)
    else:
        if args.output is None:
            parser.error('--output is required for explicit preparation')
        print(json.dumps(public_from_receipt(build(args.qt_prefix, args.output, args.archive, args.download))))
