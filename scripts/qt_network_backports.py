"""Prepare the pinned Qt Network source variant; never patch an SDK implicitly.

Only explicit preparation calls this builder. Builds and runs verify its receipt.
Original supplier binaries and metadata remain available in the owned cache.
"""
import argparse
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import time
import urllib.request

import desktop
from installer_common import ROOT, build_output, digest
import qt_source_access

RECEIPT = 'xdnote-network-backports.json'


def config():
    path = ROOT / 'tools/qt-network-backports.json'
    value = json.loads(path.read_bytes())
    if (value['qtVersion'] != desktop.DEPS['qt']['version'] or value['variant'] != desktop.DEPS['qt']['networkVariant']
            or value['sourceCodeInRelease'] is not False):
        raise ValueError('Qt Network source policy differs')
    for item in value['patches']:
        relative = Path(item['file'])
        if relative.parent != Path('tools/qt-network-patches') or digest(ROOT / relative) != item['sha256']:
            raise ValueError('Pinned Qt Network patch changed')
    return value


def headers(qt, module):
    return qt / (f'lib/Qt{module}.framework/Versions/A/Headers' if platform.system() == 'Darwin' else f'include/Qt{module}')


def features(files):
    return dict(re.findall(r'^#define QT_FEATURE_(\w+) (-?1)$', '\n'.join(p.read_text() for p in files), re.MULTILINE))


def module_paths(system):
    if system == 'Darwin':
        return ['lib/QtNetwork.framework/Versions/A/QtNetwork', 'plugins/tls/libqsecuretransportbackend.dylib']
    if system == 'Linux':
        return ['lib/libQt6Network.so.6.8.2']
    if system == 'Windows':
        return ['bin/Qt6Network.dll', 'lib/Qt6Network.lib']
    raise ValueError('Unsupported Qt Network host')


def sdk_file(qt, name):
    target = qt / name
    if target.is_symlink() or not target.resolve().is_relative_to(qt.resolve()):
        raise ValueError('SDK module path escapes its owned prefix')
    return target


def windows_compiler():
    import os
    finder = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    finder = desktop.checked_tool_path(finder, 'Visual Studio locator')
    path = subprocess.check_output([str(finder), '-latest', '-version', '[17.0,18.0)', '-products', '*',
                                    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    if not path: raise ValueError('Installed Visual Studio 2022 is required for Qt Network preparation')
    return Path(path) / 'VC/Auxiliary/Build/vcvars64.bat'


def verify(qt):
    value = config(); path = qt / RECEIPT
    if not path.is_file() or path.is_symlink():
        raise ValueError('Prepare the pinned Qt Network variant with scripts/qt_network_backports.py first')
    record = json.loads(path.read_bytes())
    if record.get('configurationSha256') != digest(ROOT / 'tools/qt-network-backports.json') or record.get('variant') != value['variant']:
        raise ValueError('Qt Network preparation is stale')
    if (record.get('producerSha256') != digest(Path(__file__))
            or record.get('compilerCompatibilitySha256') != digest(ROOT / 'cmake/QtCompilerCompatibility.h')):
        raise ValueError('Qt Network preparation recipe changed')
    required = module_paths(platform.system())
    if set(record.get('files', {})) != set(required):
        raise ValueError('Qt Network variant file set differs')
    for name in required:
        target = sdk_file(qt, name)
        if target.is_symlink() or digest(target) != record['files'][name]['sha256']:
            raise ValueError('Prepared Qt Network binary changed: ' + name)
        backup = Path(record['files'][name]['originalBackup'])
        original = desktop.checked_tool_path(backup, 'Original Qt Network backup', sdk=True)
        if backup.is_symlink() or digest(original) != record['files'][name]['originalSdkSha256']:
            raise ValueError('Original Qt Network backup changed: ' + name)
    core = headers(qt, 'Core')
    core_files = [core / 'qconfig.h', core / '6.8.2/QtCore/private/qconfig_p.h', core / '6.8.2/QtCore/private/qtcore-config_p.h']
    if record.get('coreConfiguration') != {str(p.relative_to(qt)): digest(p) for p in core_files}:
        raise ValueError('Qt Core configuration changed after Network preparation')
    return record


def public_record(qt):
    record = verify(qt)
    return {key: ({name: {k: v for k, v in row.items() if k != 'originalBackup'}
                    for name, row in value.items()} if key == 'files' else value)
            for key, value in record.items() if key not in ['coreConfiguration', 'architecture', 'system', 'originalSignature']}


def restore(qt):
    """Explicit rollback of exactly the prepared files; refuse unrelated edits."""
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True)
    record = json.loads(sdk_file(qt, RECEIPT).read_bytes())
    if set(record.get('files', {})) != set(module_paths(platform.system())):
        raise ValueError('Unexpected SDK rollback file set')
    for name, row in record['files'].items():
        if digest(sdk_file(qt, name)) != row['sha256']: raise ValueError('SDK binary has unrelated changes')
        backup = desktop.checked_tool_path(row['originalBackup'], 'Original SDK backup', sdk=True)
        if digest(backup) != row['originalSdkSha256']: raise ValueError('Original SDK backup changed')
    signature = record.get('originalSignature')
    if signature:
        backup = desktop.checked_tool_path(signature['backup'], 'Original SDK signature metadata', sdk=True)
        if digest(backup) != signature['sha256']: raise ValueError('Original SDK signature metadata changed')
    lock = qt / '.xdnote-network-prepare-lock'; lock.mkdir()
    try:
        for name, row in record['files'].items(): shutil.copy2(row['originalBackup'], sdk_file(qt, name))
        if platform.system() == 'Darwin':
            seal = sdk_file(qt, 'lib/QtNetwork.framework/Versions/A/_CodeSignature/CodeResources')
            if signature: shutil.copy2(signature['backup'], seal)
            else: seal.unlink(missing_ok=True)
        (qt / RECEIPT).unlink()
    finally: lock.rmdir()


def download_source(path, record):
    deadline = time.monotonic() + 180
    temporary = path.with_suffix(path.suffix + '.part')
    count = 0
    with urllib.request.urlopen(record['url'], timeout=20) as response, temporary.open('xb') as stream:
        while data := response.read1(65536):
            count += len(data)
            if count > 256 * 1024 * 1024 or time.monotonic() > deadline:
                raise ValueError('Qt source transfer exceeds bound')
            stream.write(data)
    if digest(temporary) != record['sha256']:
        raise ValueError('Pinned Qt source archive differs')
    temporary.rename(path)


def openssl_configure_input(env, allow_download):
    """Build only Qt's feature-test input, never a distributed TLS backend."""
    record = config()['configureDependency']
    output = ROOT / 'build/toolchain/openssl-configure' / record['version'] / platform.system()
    marker = output / 'receipt.json'
    if marker.is_file():
        saved = json.loads(marker.read_bytes())
        if saved.get('source') != record: raise ValueError('OpenSSL configure input differs')
        for name, sha in saved['files'].items():
            if digest(output / name) != sha: raise ValueError('OpenSSL configure build changed')
        return output / 'install'
    if output.exists(): raise ValueError('Preserve incomplete OpenSSL configure build')
    if not allow_download: raise ValueError('Explicit --download is required to prepare OpenSSL configure inputs')
    output.mkdir(parents=True)
    archive = output / ('openssl-' + record['version'] + '.tar.gz')
    download_source(archive, record)
    source = output / 'source'; source.mkdir()
    with tarfile.open(archive, 'r:gz') as stream: stream.extractall(source, filter='data')
    source = source / ('openssl-' + record['version'])
    system = platform.system()
    perl = desktop.prepared_tool('perl', env)
    if system == 'Windows':
        native_perl = Path('C:/Strawberry/perl/bin/perl.exe')
        if native_perl.is_file(): perl = str(desktop.checked_tool_path(native_perl, 'Native Perl'))
    target = {'Darwin': 'darwin64-arm64-cc', 'Linux': 'linux-x86_64', 'Windows': 'VC-WIN64A'}[system]
    make = desktop.prepared_tool('nmake' if system == 'Windows' else 'make', env)
    commands = [[perl, source / 'Configure', target, 'no-shared', 'no-tests', 'no-asm',
                 '--prefix=' + str(output / 'install'), '--openssldir=' + str(output / 'config')],
                [make, *([] if system == 'Windows' else ['-j4']), 'build_libs'], [make, 'install_dev']]
    for index, command in enumerate(commands):
        with (output / f'build-{index}.log').open('wb') as log:
            subprocess.run(list(map(str, command)), cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1800)
    files = {str(p.relative_to(output)): digest(p) for p in (output / 'install').rglob('*') if p.is_file()}
    marker.write_text(json.dumps({'source': record, 'commands': list(map(lambda a: list(map(str, a)), commands)), 'files': files,
                                 'scope': 'static build/configuration input only; not copied to Qt SDK or product'}, indent=2) + '\n')
    return output / 'install'


def build(qt, output, archive=None, allow_download=False, vcvars=None):
    value = config(); system = platform.system()
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True); output = build_output(output)
    if (qt / RECEIPT).exists():
        return verify(qt)
    source_record = next(r for r in qt_source_access.config()['modules'] if r['module'] == 'qtbase')
    core = headers(qt, 'Core'); network = headers(qt, 'Network')
    core_files = [core / 'qconfig.h', core / '6.8.2/QtCore/private/qconfig_p.h', core / '6.8.2/QtCore/private/qtcore-config_p.h']
    core_features = features(core_files)
    network_features = features([network / 'qtnetwork-config.h', network / '6.8.2/QtNetwork/private/qtnetwork-config_p.h'])
    expected_inputs = {'configurationSha256': digest(ROOT / 'tools/qt-network-backports.json'),
                       'producerSha256': digest(Path(__file__)), 'compilerCompatibilitySha256': digest(ROOT / 'cmake/QtCompilerCompatibility.h'),
                       'coreConfiguration': {str(p.relative_to(qt)): digest(p) for p in core_files},
                       'networkFeatures': network_features, 'system': system, 'architecture': platform.machine()}
    if output.exists():
        saved = output / 'build-receipt.json'
        if not saved.is_file(): raise ValueError('Preserve the incomplete Qt Network build; choose a new output')
        receipt = json.loads(saved.read_bytes())
        if receipt.get('inputs') != expected_inputs: raise ValueError('Cached Qt Network inputs differ')
        for name, row in receipt['files'].items():
            if digest(output / row['builtFile']) != row['sha256']: raise ValueError('Cached Qt Network binary differs')
    else:
        output.mkdir(parents=True)
        if archive is None:
            archive = output / source_record['archive']
            if not allow_download: raise ValueError('Provide a checked source archive or explicitly allow download')
            download_source(archive, source_record)
        archive = build_output(archive)
        if digest(archive) != source_record['sha256']: raise ValueError('Pinned Qt source archive differs')
        source = output / 'source'; source.mkdir()
        with tarfile.open(archive, 'r:xz') as stream: stream.extractall(source, filter='data')
        source = source / 'qtbase-everywhere-src-6.8.2'
        patches = [p for p in value['patches'] if system in p.get('platforms', ['Darwin', 'Linux', 'Windows'])]
        env = desktop.environment(vcvars or (windows_compiler() if system == 'Windows' else None))
        for name in list(env):
            if name.upper().startswith(('QT_', 'QML_', 'LD_', 'DYLD_')) or name.upper() in {'CMAKE_PREFIX_PATH', 'CMAKE_MODULE_PATH', 'QTDIR', 'PKG_CONFIG_PATH', 'LIBRARY_PATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH', 'CC', 'CXX'}:
                env.pop(name)
        openssl = openssl_configure_input(env, allow_download)
        commands = []
        def run(args, name, timeout=1800):
            commands.append(list(map(str, args)))
            with (output / name).open('wb') as log:
                subprocess.run(list(map(str, args)), cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=timeout)
        for index, patch in enumerate(patches):
            args = ['git', 'apply', '--directory', str(source.relative_to(ROOT)), ROOT / patch['file']]
            run(args[:2] + ['--check'] + args[2:], f'patch-{index}-check.log')
            run(args, f'patch-{index}.log')
        cmake = desktop.prepared_tool('cmake', env); ninja = desktop.prepared_tool('ninja', env)
        args = [cmake, '-S', source, '-B', output / 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_INSTALL_PREFIX=' + str(output / 'unused-install'),
                '-DBUILD_SHARED_LIBS=ON', '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF',
                '-DFEATURE_gui=OFF', '-DFEATURE_network=ON', '-DFEATURE_sql=OFF', '-DFEATURE_dbus=OFF',
                '-DFEATURE_system_pcre2=OFF', '-DFEATURE_system_libb2=OFF', '-DFEATURE_system_doubleconversion=OFF']
        args += ['-DFEATURE_' + k + '=' + ('ON' if core_features[k] == '1' else 'OFF') for k in ['system_zlib', 'zstd', 'icu']]
        args += ['-DOPENSSL_ROOT_DIR=' + str(openssl), '-DOPENSSL_USE_STATIC_LIBS=TRUE',
                 '-DFEATURE_openssl=ON', '-DFEATURE_openssl_linked=OFF', '-DFEATURE_openssl_runtime=ON', '-DFEATURE_openssl_hash=OFF']
        args += ['-DFEATURE_' + k + '=' + ('ON' if v == '1' else 'OFF') for k, v in network_features.items()]
        if system == 'Darwin':
            desktop.check_host(system, platform.machine(), platform.mac_ver()[0])
            yield_header = source / 'src/corelib/thread/qyieldcpu.h'
            yield_header.write_text('#if defined(__aarch64__)\n#include <arm_acle.h>\n#endif\n' + yield_header.read_text())
            compatibility = ROOT / 'cmake/QtCompilerCompatibility.h'
            args += ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0',
                     '-DCMAKE_CXX_COMPILER=/usr/bin/clang++', '-DCMAKE_C_COMPILER=/usr/bin/clang',
                     '-DCMAKE_CXX_FLAGS=-include "' + str(compatibility) + '"', '-DCMAKE_OBJCXX_FLAGS=-include "' + str(compatibility) + '"']
            if subprocess.run(['/usr/bin/xcodebuild', '-version'], capture_output=True).returncode:
                args += ['-DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON']
        run(args, 'configure.log')
        targets = ['Network', 'QSecureTransportBackendPlugin'] if system == 'Darwin' else ['Network']
        run([cmake, '--build', output / 'build', '--target', *targets, '--parallel', '4'], 'build.log', 3600)
        actual_features = features([output / 'build/src/network/qtnetwork-config.h', output / 'build/src/network/qtnetwork-config_p.h'])
        if actual_features != network_features: raise ValueError('Built Qt Network features differ from the fixed SDK')
        receipt = {'inputs': expected_inputs, 'variant': value['variant'], 'source': source_record,
                   'patches': patches, 'commands': commands, 'files': {}, 'sourceCodeInRelease': False,
                   'licenseReviewComplete': False, 'releaseReady': False}
        for name in module_paths(system):
            relative = 'build/' + name
            receipt['files'][name] = {'builtFile': relative, 'sha256': digest(output / relative)}
        (output / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    install(qt, output, receipt)
    return verify(qt)


def install(qt, output, build_receipt):
    if set(build_receipt['files']) != set(module_paths(platform.system())):
        raise ValueError('Unexpected prepared SDK module set')
    lock = qt / '.xdnote-network-prepare-lock'; lock.mkdir()
    backups = output / 'original-sdk'; backups.mkdir(exist_ok=True)
    originals = {}
    try:
        for name in build_receipt['files']:
            path = sdk_file(qt, name)
            if path.is_symlink() or not path.is_file(): raise ValueError('Ordinary SDK binary required: ' + name)
            copy = backups / name; copy.parent.mkdir(parents=True, exist_ok=True)
            if not copy.exists(): shutil.copy2(path, copy)
            if digest(copy) != digest(path): raise ValueError('Original SDK backup differs')
            originals[name] = digest(path)
        signature = 'lib/QtNetwork.framework/Versions/A/_CodeSignature/CodeResources'
        if platform.system() == 'Darwin' and (qt / signature).is_file():
            p = backups / signature; p.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(qt / signature, p)
        for name, row in build_receipt['files'].items():
            temporary = sdk_file(qt, name).with_name(Path(name).name + '.xdnote-new')
            shutil.copy2(output / row['builtFile'], temporary); temporary.replace(qt / name)
        if platform.system() == 'Darwin':
            for path in [qt / 'lib/QtNetwork.framework', qt / 'plugins/tls/libqsecuretransportbackend.dylib']:
                subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(path)], check=True, capture_output=True)
                subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)], check=True, capture_output=True)
        record = {'schemaVersion': 1, **build_receipt['inputs'], 'variant': build_receipt['variant'],
                  'source': build_receipt['source'], 'patches': build_receipt['patches'], 'files': {},
                  'sourceBuildReceiptSha256': digest(output / 'build-receipt.json'),
                  'sourceCodeInRelease': False, 'licenseReviewComplete': False, 'releaseReady': False}
        if platform.system() == 'Darwin' and (backups / signature).is_file():
            record['originalSignature'] = {'backup': str(backups / signature), 'sha256': digest(backups / signature)}
        for name in build_receipt['files']:
            record['files'][name] = {'sha256': digest(qt / name), 'originalSdkSha256': originals[name],
                                      'originalBackup': str(backups / name)}
        (qt / RECEIPT).write_text(json.dumps(record, indent=2) + '\n')
    except BaseException:
        for name in originals: shutil.copy2(backups / name, qt / name)
        if platform.system() == 'Darwin':
            if (backups / signature).is_file(): shutil.copy2(backups / signature, qt / signature)
            elif (qt / signature).exists(): (qt / signature).unlink()
        (qt / RECEIPT).unlink(missing_ok=True)
        raise
    finally:
        for name in build_receipt['files']:
            (qt / name).with_name(Path(name).name + '.xdnote-new').unlink(missing_ok=True)
        lock.rmdir()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qt-prefix', type=Path, required=True); p.add_argument('--output', type=Path)
    p.add_argument('--archive', type=Path); p.add_argument('--download', action='store_true'); p.add_argument('--vcvars', type=Path)
    p.add_argument('--restore-original', action='store_true')
    a = p.parse_args()
    if a.restore_original: restore(a.qt_prefix); print('Restored original supplier SDK files')
    else:
        if not a.output: p.error('--output is required for explicit preparation')
        print(json.dumps(build(a.qt_prefix, a.output, a.archive, a.download, a.vcvars)))
