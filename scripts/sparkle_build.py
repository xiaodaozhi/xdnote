"""Rebuild the pinned Sparkle framework library, retaining upstream helpers/tools.

The source and SDK archives are authenticated by prepare-sparkle before entry.
All 67 upstream framework translation units and the 10 upstream Ed25519 units
are compiled natively. Installer binaries, resources and signing tools retain
their pinned SDK origin; this is not a rebuild claim for those components.
"""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs(spec):
    variant = spec['framework_build']
    patch = ROOT / variant['patch']
    if digest(patch) != variant['patch_sha256']:
        raise ValueError('Sparkle proxy patch differs from its pinned identity')
    return {'variant': variant['variant'], 'source_sha256': spec['source']['sha256'],
            'sdk_sha256': spec['sdk']['sha256'], 'patch_sha256': digest(patch),
            'builder_sha256': digest(__file__), 'architecture': platform.machine(),
            'minimum_macos': '14.0'}


def source_files(project, target, root):
    """Resolve the pinned Xcode source phases, without requiring Xcode itself."""
    objects = project['objects']
    parents = {child: identity for identity, node in objects.items()
               for child in node.get('children', [])}

    def resolve(identity):
        node = objects[identity]
        if node.get('sourceTree') not in ('<group>', 'SOURCE_ROOT', None):
            raise ValueError('Unexpected Sparkle source tree')
        parent = (resolve(parents[identity]) if identity in parents and
                  node.get('sourceTree') != 'SOURCE_ROOT' else root)
        path = (parent / node.get('path', '')).resolve()
        if not path.is_relative_to(root.resolve()):
            raise ValueError('Sparkle source escapes its pinned tree')
        return path

    selected = next(node for node in objects.values()
                    if node.get('isa') == 'PBXNativeTarget' and node.get('name') == target)
    phase = next(objects[identity] for identity in selected['buildPhases']
                 if objects[identity]['isa'] == 'PBXSourcesBuildPhase')
    result = []
    for identity in phase['files']:
        entry = objects[identity]
        if entry.get('settings'):
            raise ValueError('Unreviewed Sparkle per-file compiler settings')
        path = resolve(entry['fileRef'])
        if path.is_symlink() or not path.is_file() or path.suffix not in ('.m', '.c'):
            raise ValueError('Unexpected Sparkle translation unit')
        result.append(path)
    return result


def definitions():
    # Match ConfigCommon.xcconfig and ConfigFramework.xcconfig in pinned 2.10.0.
    result = {name: '1' for name in (
        'SPARKLE_BUILD_UI_BITS', 'SPARKLE_COPY_LOCALIZATIONS',
        'SPARKLE_BUILD_LEGACY_SUUPDATER', 'SPARKLE_BUILD_PACKAGE_SUPPORT',
        'SPARKLE_BUILD_LEGACY_DELTA_SUPPORT', 'SPARKLE_BUILD_BZIP2_DELTA_SUPPORT',
        'SPARKLE_BUILD_LEGACY_DSA_SUPPORT', 'GENERATE_APPCAST_BUILD_LEGACY_DSA_SUPPORT',
        'INSTALLER_LAUNCHER_XPC_SERVICE_EMBEDDED', 'DOWNLOADER_XPC_SERVICE_EMBEDDED',
        'BUILDING_SPARKLE', 'NDEBUG')}
    result.update({name: '0' for name in (
        'SPARKLE_NORMALIZE_INSTALLED_APPLICATION_NAME',
        'INSTALLER_CONNECTION_XPC_SERVICE_EMBEDDED', 'INSTALLER_STATUS_XPC_SERVICE_EMBEDDED')})
    result.update({'SPU_OBJC_DIRECT': '__attribute__((objc_direct))',
                   'SPU_OBJC_DIRECT_MEMBERS': '__attribute__((objc_direct_members))'})
    strings = {
        'SPARKLE_BUNDLE_IDENTIFIER': 'org.sparkle-project.Sparkle',
        'CURRENT_PROJECT_VERSION': '2064', 'MARKETING_VERSION': '2.10.0',
        'SPARKLE_RELAUNCH_TOOL_NAME': 'Autoupdate',
        'SPARKLE_INSTALLER_PROGRESS_TOOL_NAME': 'Updater',
        'SPARKLE_INSTALLER_PROGRESS_TOOL_BUNDLE_ID': 'org.sparkle-project.Sparkle.Updater',
        'SPARKLE_ICON_NAME': 'AppIcon',
        'INSTALLER_LAUNCHER_NAME': 'Installer',
        'INSTALLER_LAUNCHER_BUNDLE_ID': 'org.sparkle-project.InstallerLauncher',
        'INSTALLER_CONNECTION_NAME': 'InstallerConnection',
        'INSTALLER_CONNECTION_BUNDLE_ID': 'org.sparkle-project.InstallerConnection',
        'INSTALLER_STATUS_NAME': 'InstallerStatus',
        'INSTALLER_STATUS_BUNDLE_ID': 'org.sparkle-project.InstallerStatus',
        'DOWNLOADER_NAME': 'Downloader',
        'DOWNLOADER_BUNDLE_ID': 'org.sparkle-project.DownloaderService'}
    result.update({name: '"' + value + '"' for name, value in strings.items()})
    return ['-D' + name + '=' + value for name, value in result.items()]


def build(source, sdk, work, spec, log):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise ValueError('Sparkle library builds require native Apple Silicon macOS')
    identity = inputs(spec)
    work.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_bytes(b'')

    def invoke(arguments, *, cwd=None):
        result = subprocess.run(list(map(str, arguments)), cwd=cwd, capture_output=True)
        data = (result.stdout + result.stderr).replace(str(ROOT).encode(), b'<workspace>')
        data = data.replace(str(Path.home()).encode(), b'<user>')
        return result, data

    def checked(arguments, *, cwd=None, structured=False):
        result, data = invoke(arguments, cwd=cwd)
        with log.open('ab') as output:
            output.write(result.stderr if structured else data)
        if result.returncode:
            raise ValueError('Sparkle library build failed; see its local build log')
        return result.stdout

    checked(['/usr/bin/patch', '--batch', '--fuzz=0', '-p1', '-i', ROOT / spec['framework_build']['patch']], cwd=source)
    project = json.loads(checked(['/usr/bin/plutil', '-convert', 'json', '-o', '-', source / 'Sparkle.xcodeproj/project.pbxproj'], structured=True))
    framework_sources = source_files(project, 'Sparkle', source)
    crypto_sources = source_files(project, 'ed25519', source)
    if len(framework_sources) != 67 or len(crypto_sources) != 10:
        raise ValueError('Sparkle build phases differ from the reviewed source set')
    toolchain = {
        'compiler': checked(['/usr/bin/xcrun', 'clang', '--version']).decode().strip(),
        'sdkVersion': checked(['/usr/bin/xcrun', '--show-sdk-version']).decode().strip(),
        'hostMacos': platform.mac_ver()[0],
    }
    common = ['/usr/bin/xcrun', 'clang', '-arch', platform.machine(), '-mmacosx-version-min=14.0',
              '-Os', '-fvisibility=hidden', '-fblocks', '-fmodules',
              '-fmodules-cache-path=' + str(work / 'modules'),
              '-Wall', '-Werror=implicit-function-declaration', '-Werror=unguarded-availability-new']
    for directory in ('Sparkle', 'Autoupdate', 'Downloader', 'InstallerConnection',
                      'InstallerLauncher', 'InstallerStatus', 'Vendor/ed25519-sparkle/src', '.'):
        common += ['-I', str(source / directory)]
    common += ['-F', str(sdk)]

    def compile_one(path):
        target = work / (path.relative_to(source).as_posix().replace('/', '_') + '.o')
        arguments = common + (['-fobjc-arc'] + definitions() if path.suffix == '.m' else [])
        result, data = invoke(arguments + ['-c', path, '-o', target])
        return target, result.returncode, data

    with ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 1)) as pool:
        results = list(pool.map(compile_one, framework_sources + crypto_sources))
    with log.open('ab') as output:
        for _, _, data in results:
            output.write(data)
    if any(code for _, code, _ in results):
        raise ValueError('Sparkle translation unit failed; see its local build log')
    library = sdk / 'Sparkle.framework/Versions/B/Sparkle'
    arguments = ['/usr/bin/xcrun', 'clang', '-arch', platform.machine(), '-mmacosx-version-min=14.0',
                 '-dynamiclib', '-install_name', '@rpath/Sparkle.framework/Versions/B/Sparkle',
                 '-compatibility_version', '1.6', '-current_version', '2.10.0',
                 '-o', library] + [target for target, _, _ in results]
    for framework in ('Foundation', 'AppKit', 'CoreFoundation', 'CoreGraphics', 'IOKit',
                      'Security', 'ServiceManagement', 'SystemConfiguration', 'WebKit', 'Network', 'CFNetwork'):
        arguments += ['-framework', framework]
    checked(arguments + ['-lcompression'])
    shutil.copyfile(source / 'Sparkle/SPUUpdater.h', sdk / 'Sparkle.framework/Versions/B/Headers/SPUUpdater.h')
    # The upstream dSYM for the framework library no longer describes this code.
    if (sdk / 'Symbols').is_dir():
        shutil.rmtree(sdk / 'Symbols')
    info_path = sdk / 'Sparkle.framework/Versions/B/Resources/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    info['XdnoteFrameworkBuildVariant'] = identity['variant']
    info_path.write_bytes(plistlib.dumps(info, sort_keys=False))
    # Only the modified framework seal is recreated. Upstream helper code and
    # its entitlements are retained; deployment signs each helper separately.
    checked(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', '--options', 'runtime', sdk / 'Sparkle.framework'])
    checked(['/usr/bin/codesign', '--verify', '--deep', '--strict', sdk / 'Sparkle.framework'])
    return dict(identity, framework_translation_units=len(framework_sources),
                ed25519_translation_units=len(crypto_sources),
                toolchain=toolchain,
                translation_units={path.relative_to(source).as_posix(): digest(path)
                                   for path in framework_sources + crypto_sources},
                library_sha256=digest(library), helper_origin='pinned upstream SDK, unchanged',
                files={path.relative_to(sdk).as_posix(): digest(path)
                       for path in sorted((sdk / 'Sparkle.framework').rglob('*'))
                       if path.is_file() and not path.is_symlink()})
