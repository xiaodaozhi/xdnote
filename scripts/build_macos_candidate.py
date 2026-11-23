"""Package an audited macOS arm64 app as a candidate ZIP and disk image."""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import subprocess
import tempfile

from installer_common import build_output, digest
from product_deployment_unix import inventory, require_product
from release_candidate import TARGETS, validate_identity


def macos_tool(name, *args):
    return subprocess.check_output(['/usr/bin/' + name, *map(str, args)], timeout=180)


def verify_app(app, identity, files):
    if app.is_symlink() or not app.is_dir() or inventory(app) != files:
        raise ValueError('Packaged app differs from the sealed app inventory')
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    expected = {
        'CFBundleIdentifier': 'io.github.xiaodaozhi.xdnote',
        'CFBundleExecutable': 'xdnote',
        'CFBundleShortVersionString': identity['version'],
        'CFBundleVersion': str(identity['build']),
        'LSMinimumSystemVersion': '14.0',
    }
    if any(info.get(key) != value for key, value in expected.items()):
        raise ValueError('App bundle identity differs from the candidate')
    binary = app / 'Contents/MacOS/xdnote'
    if not os.access(binary, os.X_OK) or macos_tool('lipo', '-archs', binary).split() != [b'arm64']:
        raise ValueError('The candidate must contain an executable arm64 main program')
    macos_tool('codesign', '--verify', '--deep', '--strict', app)


def package(root, audit, output):
    if output.exists() or output.is_relative_to(root):
        raise ValueError('Use a new candidate output directory outside the sealed payload')
    if (audit.get('ok') is not True or audit.get('kind') != 'product-only Unix developer payload'
            or audit.get('host') != {'system': 'Darwin', 'architecture': 'arm64'}
            or {p.name for p in root.iterdir()} != {'xdnote.app'}
            or audit.get('files') != inventory(root)):
        raise ValueError('An audited and unchanged macOS arm64 payload is required')
    require_product(root)
    identity = validate_identity(audit['buildIdentity']['buildVersion'])
    app = root / 'xdnote.app'
    files = inventory(app)
    verify_app(app, identity, files)
    suffixes = {kind: suffix for system, arch, kind, suffix, _ in TARGETS if (system, arch) == ('macos', 'arm64')}
    prefix = 'xdnote-' + identity['version'] + '-'
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.macos-candidate-', dir=output.parent) as temporary:
        work = Path(temporary)
        assets = work / 'verified'; assets.mkdir()
        archive = assets / (prefix + suffixes['app.zip'])
        image = assets / (prefix + suffixes['dmg'])
        macos_tool('ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, archive)
        macos_tool('ditto', '-x', '-k', archive, work / 'expanded')
        if {p.name for p in (work / 'expanded').iterdir()} != {'xdnote.app'}:
            raise ValueError('Unexpected ZIP contents')
        verify_app(work / 'expanded/xdnote.app', identity, files)

        stage = work / 'image'; stage.mkdir()
        macos_tool('ditto', app, stage / app.name)
        (stage / 'Applications').symlink_to('/Applications')
        macos_tool('hdiutil', 'create', '-volname', 'xdnote', '-srcfolder', stage,
                   '-format', 'UDZO', '-fs', 'HFS+', image)
        macos_tool('hdiutil', 'verify', image)
        mount = work / 'mount'; mount.mkdir()
        try:
            macos_tool('hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', mount, image)
            shortcut = mount / 'Applications'
            if not shortcut.is_symlink() or os.readlink(shortcut) != '/Applications':
                raise ValueError('DMG Applications shortcut is missing')
            verify_app(mount / app.name, identity, files)
        finally:
            if os.path.ismount(mount):
                macos_tool('hdiutil', 'detach', mount)

        evidence = {
            'identity': identity,
            'scope': 'macOS candidate packaging and bundle integrity',
            'payloadFiles': len(files),
            'assets': {path.name: {'sha256': digest(path), 'bytes': path.stat().st_size, 'verified': True}
                       for path in (archive, image)},
            'releaseReady': False,
        }
        (assets / 'verification.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
        output.mkdir()
        try:
            assets.replace(output)
        except OSError:
            output.rmdir()
            raise
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'audit', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if (platform.system(), platform.machine()) != ('Darwin', 'arm64'):
        parser.error('Requires an Apple Silicon macOS host')
    try:
        root, audit, output = map(build_output, (args.root, args.audit, args.output_dir))
        result = package(root, json.loads(audit.read_bytes()), output)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
