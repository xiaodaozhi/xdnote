"""Prepare unsigned schema-1 candidate metadata from six actual, verified assets.

This command never signs, uploads, tags, creates a Release or changes trust roots.
"""
import argparse
import json
from pathlib import Path
import re
from build_version import public_version
from installer_common import build_output, digest

MAX_ASSET_BYTES = 2 * 1024**3
TARGETS = (
    ('windows', 'x86_64', 'zip', 'windows-x86_64.zip', {'windows': '10.0.17763'}),
    ('windows', 'x86_64', 'exe', 'windows-x86_64-setup.exe', {'windows': '10.0.17763'}),
    ('macos', 'arm64', 'app.zip', 'macos-arm64.app.zip', {'macos': '14.0'}),
    ('macos', 'arm64', 'dmg', 'macos-arm64.dmg', {'macos': '14.0'}),
    ('linux', 'x86_64', 'tar.gz', 'linux-x86_64.tar.gz', {'distribution': 'ubuntu', 'version': '22.04', 'glibc': '2.35', 'display_server': 'Wayland/X11'}),
    ('linux', 'x86_64', 'deb', 'ubuntu22.04-x86_64.deb', {'distribution': 'ubuntu', 'version': '22.04', 'glibc': '2.35', 'display_server': 'Wayland/X11'}),
)


def validate_identity(identity):
    public_version(identity['version'])
    if (identity.get('product') != 'xdnote' or identity.get('dirty') is not False or identity.get('configuration') != 'Release'
            or not re.fullmatch('[0-9a-f]{40}', identity.get('revision', '')) or type(identity.get('build')) is not int
            or not 1 <= identity['build'] <= 65535):
        raise ValueError('A clean explicit release identity is required')
    channel = identity['version'].split('-')[1].split('.')[0] if '-' in identity['version'] else 'stable'
    if identity.get('channel') != channel: raise ValueError('Release channel differs')
    return identity


def manifest(identity, directory, verification):
    validate_identity(identity)
    if verification.get('identity') != identity: raise ValueError('Asset evidence identity differs')
    assets = []
    version = identity['version']; tag = 'v' + version
    for platform, arch, package, suffix, minimum in TARGETS:
        name = 'xdnote-' + version + '-' + suffix; path = directory / name
        if path.is_symlink() or not path.is_file() or not 1 <= path.stat().st_size <= MAX_ASSET_BYTES: raise ValueError('Missing or invalid asset: ' + name)
        sha = digest(path); evidence = verification.get('assets', {}).get(name, {})
        if evidence.get('sha256') != sha or evidence.get('bytes') != path.stat().st_size or evidence.get('verified') is not True:
            raise ValueError('Asset verification missing or stale: ' + name)
        assets.append(dict(filename=name, platform=platform, architecture=arch, package_type=package, minimum_system=minimum,
                           bytes=path.stat().st_size, sha256=sha, url=f'https://github.com/xiaodaozhi/xdnote/releases/download/{tag}/{name}'))
    return dict(schema_version=1, product='xdnote', version=version, channel=identity['channel'], build=identity['build'],
                tag=tag, commit=identity['revision'], repository='xiaodaozhi/xdnote', assets=assets,
                macos_appcast_url=f'https://github.com/xiaodaozhi/xdnote/releases/download/{tag}/appcast.xml')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('identity', 'assets', 'verification', 'output'): p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); output = build_output(a.output)
    if output.exists(): p.error('Preserve an existing candidate')
    result = manifest(json.loads(a.identity.read_bytes()), build_output(a.assets), json.loads(a.verification.read_bytes()))
    output.parent.mkdir(parents=True, exist_ok=True); output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print('Unsigned candidate prepared; authentication and release acceptance remain separate.')
