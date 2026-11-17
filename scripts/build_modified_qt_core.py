"""Rebuild a source-modified Qt Core in an owned cache, without installing it.

The archive is explicit and must match the project's pinned official source.
Only a proof export and the documented Apple compiler compatibility input change.
"""
import argparse
import difflib
import json
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import desktop
import qt_source_access
from installer_common import ROOT, build_output, digest

PROOF = 'xdnote-qt-source-rebuild-6.8.2'


def sdk_features(qt, system):
    headers = qt / ('lib/QtCore.framework/Versions/A/Headers' if system == 'Darwin' else 'include/QtCore')
    files = [headers / 'qconfig.h', headers / '6.8.2/QtCore/private/qtcore-config_p.h']
    text = '\n'.join(p.read_text(encoding='utf-8') for p in files)
    result = {}
    for name in ['zstd', 'icu']:
        match = re.search(r'^#define QT_FEATURE_' + name + r' (-?1)$', text, re.MULTILINE)
        if not match: raise ValueError('SDK feature metadata missing: ' + name)
        result[name] = match[1] == '1'
    return result


def build(archive, qt, output, vcvars=None):
    system = platform.system(); env = desktop.environment(vcvars)
    for name in list(env):
        if name.upper().startswith(('QT_', 'QML_', 'LD_', 'DYLD_')) or name.upper() in {'CMAKE_PREFIX_PATH', 'CMAKE_MODULE_PATH', 'QTDIR', 'PKG_CONFIG_PATH', 'LIBRARY_PATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH', 'CC', 'CXX'}:
            env.pop(name)
    qt = desktop.checked_tool_path(qt, 'Qt SDK', sdk=True); archive = build_output(archive); output = build_output(output)
    record = next(r for r in qt_source_access.config()['modules'] if r['module'] == 'qtbase')
    sdk_relative = {'Darwin': 'lib/QtCore.framework/Versions/A/QtCore', 'Linux': 'lib/libQt6Core.so.6.8.2', 'Windows': 'bin/Qt6Core.dll'}[system]
    sdk_hash = digest(qt / sdk_relative)
    if digest(archive) != record['sha256']: raise ValueError('Pinned Qt source archive differs')
    if output.exists(): raise ValueError('Preserve existing source proof build')
    output.mkdir(parents=True); source = output / 'source'; source.mkdir()
    with tarfile.open(archive, 'r:xz') as stream: stream.extractall(source, filter='data')
    source = source / 'qtbase-everywhere-src-6.8.2'
    edits = []
    def edit(relative, transform):
        path = source / relative; before = path.read_text(encoding='utf-8'); after = transform(before)
        path.write_text(after, encoding='utf-8', newline='\n')
        edits.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='a/' + relative, tofile='b/' + relative))
    edit('src/corelib/global/qglobal.cpp', lambda s: s + '\nextern "C" Q_DECL_EXPORT const char *xdnoteQtSourceProof() { return "' + PROOF + '"; }\n')
    features = sdk_features(qt, system)
    commands = []; cmake = desktop.prepared_tool('cmake', env); ninja = desktop.prepared_tool('ninja', env)
    args = [cmake, '-S', source, '-B', output / 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MAKE_PROGRAM=' + ninja,
            '-DCMAKE_INSTALL_PREFIX=' + str(output / 'unused-install'), '-DBUILD_SHARED_LIBS=ON', '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF',
            '-DFEATURE_gui=OFF', '-DFEATURE_network=OFF', '-DFEATURE_sql=OFF', '-DFEATURE_dbus=OFF',
            '-DFEATURE_system_pcre2=OFF', '-DFEATURE_system_zlib=OFF', '-DFEATURE_system_libb2=OFF', '-DFEATURE_system_doubleconversion=OFF']
    args += ['-DFEATURE_' + name + '=' + ('ON' if enabled else 'OFF') for name, enabled in features.items()]
    if system == 'Linux': args += ['-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_C_COMPILER=/usr/bin/gcc']
    if system == 'Darwin':
        desktop.check_host(system, platform.machine(), platform.mac_ver()[0])
        edit('src/corelib/thread/qyieldcpu.h', lambda s: '#if defined(__aarch64__)\n#include <arm_acle.h>\n#endif\n' + s)
        compatibility = ROOT / 'cmake/QtCompilerCompatibility.h'
        args += ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0',
                 '-DCMAKE_CXX_COMPILER=/usr/bin/clang++', '-DCMAKE_C_COMPILER=/usr/bin/clang',
                 '-DCMAKE_CXX_FLAGS=-include "' + str(compatibility) + '"', '-DCMAKE_OBJCXX_FLAGS=-include "' + str(compatibility) + '"']
        version = subprocess.run(['/usr/bin/xcodebuild', '-version'], capture_output=True)
        if version.returncode: args += ['-DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON']
    elif system not in ('Linux', 'Windows'): raise ValueError('Unsupported native host')
    def run(command, log):
        commands.append(list(map(str, command)))
        with (output / log).open('wb') as stream:
            subprocess.run(list(map(str, command)), env=env, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=1800)
    run(args, 'configure.log'); run([cmake, '--build', output / 'build', '--target', 'Core', '--parallel', '4'], 'build.log')
    relative = {'Darwin': 'lib/QtCore.framework/Versions/A/QtCore', 'Linux': 'lib/libQt6Core.so.6.8.2', 'Windows': 'bin/Qt6Core.dll'}[system]
    library = output / 'build' / relative
    if library.is_symlink() or not library.is_file(): raise ValueError('Source-built actual Core binary missing')
    if digest(qt / sdk_relative) != sdk_hash: raise ValueError('Fixed SDK changed during source build')
    patch = output / 'source.patch'; patch.write_text(''.join(edits), encoding='utf-8', newline='\n')
    result = {'ok': True, 'source': record, 'archiveSha256': digest(archive), 'sourcePatchSha256': digest(patch), 'sdkFeatures': features,
              'commands': commands, 'library': str(library), 'librarySha256': digest(library), 'proof': PROOF,
              'system': system, 'architecture': platform.machine(), 'installed': False, 'fixedSdkChanged': False, 'fixedSdkQtCoreSha256': sdk_hash, 'releaseReady': False,
              'scope': 'modified Qt Core source build only; not a full supplier rebuild or final corresponding-source obligation determination'}
    (output / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['archive', 'qt-prefix', 'output']: p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--vcvars', type=Path); a = p.parse_args()
    print(json.dumps(build(a.archive, a.qt_prefix, a.output, a.vcvars)))
