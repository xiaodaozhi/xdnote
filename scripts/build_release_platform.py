"""Build the two unsigned candidate packages for Windows x64 or Ubuntu 22.04 x64."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

import developer_archive
from installer_common import ROOT, build_output, digest
from product_deployment_unix import inventory
from release_candidate import MAX_ASSET_BYTES, TARGETS, validate_identity


def check_debian(package, staged, extracted, version):
    result = subprocess.check_output(['/usr/bin/dpkg-deb', '--field', package], text=True, timeout=30)
    fields = dict(line.split(': ', 1) for line in result.splitlines() if line and not line[0].isspace())
    if (fields.get('Package'), fields.get('Version'), fields.get('Architecture')) != ('xdnote', version, 'amd64'):
        raise ValueError('Debian package metadata differs from the sealed build')
    subprocess.run(['/usr/bin/dpkg-deb', '--extract', package, extracted], check=True, timeout=120)
    expected = [entry for entry in inventory(staged) if not entry['path'].startswith('DEBIAN/')]
    if inventory(extracted) != expected:
        raise ValueError('Extracted Debian package differs from its staged application and launcher')


def asset_receipt(path, identity, report, name_field):
    if (report.get('ok') is not True or report.get('buildIdentity', {}).get('buildVersion') != identity
            or report.get(name_field) != path.name or path.is_symlink() or not path.is_file()):
        raise ValueError('Package receipt does not bind the candidate: ' + path.name)
    size = path.stat().st_size
    checksum = digest(path)
    if not 1 <= size <= MAX_ASSET_BYTES or checksum != report.get('sha256') or report.get('bytes', size) != size:
        raise ValueError('Package bytes differ from the builder receipt: ' + path.name)
    return {'bytes': size, 'sha256': checksum, 'verified': True}


def package(root, audit_path, compatibility, output, toolchain):
    system = platform.system()
    machine = platform.machine().lower()
    if system not in ('Windows', 'Linux') or machine not in ('amd64', 'x86_64'):
        raise ValueError('Use a native Windows x64 or Ubuntu 22.04 x64 build host')
    windows = system == 'Windows'
    if windows != (toolchain is not None):
        raise ValueError('Provide the pinned installer toolchain for Windows only')
    if not windows:
        os_release = platform.freedesktop_os_release()
        if (os_release.get('ID'), os_release.get('VERSION_ID')) != ('ubuntu', '22.04'):
            raise ValueError('Build Debian packages on the Ubuntu 22.04 reference host')
    if output.exists() or output.is_relative_to(root):
        raise ValueError('Use a new candidate output directory outside the payload')
    audit = json.loads(audit_path.read_bytes())
    identity = validate_identity(audit['buildIdentity']['buildVersion'])
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if revision != identity['revision'] or subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT):
        raise ValueError('Build packages from the exact clean candidate commit')
    expected_kind = 'unsigned product-only Windows installer payload' if windows else 'product-only Unix developer payload'
    if audit.get('kind') != expected_kind or (compatibility.get('system'), compatibility.get('architecture')) != (system, 'x86_64'):
        raise ValueError('Use sealed product and compatibility reports for the native target')
    suffixes = [suffix for target, _, _, suffix, _ in TARGETS if target == system.lower()]
    prefix = 'xdnote-' + identity['version'] + '-'
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.platform-candidate-', dir=output.parent) as temporary:
        work = Path(temporary)
        products = work / 'packages'
        products.mkdir()
        archive, installer = (work / (prefix + suffix) for suffix in suffixes)
        archived = developer_archive.build(root, audit, compatibility, archive)
        if not archived['archiveReproducible'] or not archived['extractedContentMatches']:
            raise ValueError('The archive did not pass sealed payload verification')
        installer_receipt = work / 'installer.json'
        native_script = 'build-windows-installer.py' if windows else 'build_linux_installer.py'
        args = [sys.executable, ROOT / 'scripts' / native_script, '--root', root, '--audit', audit_path,
                '--output', installer, '--report', installer_receipt]
        if windows:
            args += ['--toolchain', toolchain]
        subprocess.run(args, check=True, timeout=240)
        installed = json.loads(installer_receipt.read_bytes())
        if windows:
            if installed.get('qaFixture') is not False or installed.get('signed') is not False:
                raise ValueError('Expected an unsigned product installer without fixture payloads')
        else:
            check_debian(installer, installer.with_suffix('.stage'), work / 'extracted', installed['version'])
        developer_archive.verify(root, audit)
        assets = {archive.name: asset_receipt(archive, identity, archived, 'archive'),
                  installer.name: asset_receipt(installer, identity, installed, 'file')}
        result = {'identity': identity, 'scope': 'native packaging and sealed payload verification',
                  'assets': assets, 'releaseReady': False}
        for asset in (archive, installer):
            asset.rename(products / asset.name)
        reports = products / 'reports'
        reports.mkdir()
        archive.with_name(archive.name + '.manifest.json').rename(reports / 'archive.json')
        installer_receipt.rename(reports / 'installer.json')
        (products / 'verification.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        if output.exists() or output.is_symlink():
            raise ValueError('Preserve the existing candidate output directory')
        products.rename(output)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'audit', 'compatibility', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    a = parser.parse_args()
    try:
        result = package(build_output(a.root), build_output(a.audit), json.loads(build_output(a.compatibility).read_bytes()),
                         build_output(a.output_dir), build_output(a.toolchain) if a.toolchain else None)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
