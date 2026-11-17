"""Record source identity; release inputs must explicitly bind a clean commit."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def public_version(value):
    match = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(alpha|beta|rc)\.([1-9][0-9]*))?', value)
    if not match or any(int(n) > 65535 for n in match.group(1, 2, 3)) or (match[5] and int(match[5]) > 65535):
        raise ValueError('Invalid public version')
    return (*map(int, match.group(1, 2, 3)), {'alpha': 0, 'beta': 1, 'rc': 2, None: 3}[match[4]], int(match[5] or 0))


def release_identity(value, revision, dirty, configuration):
    required = {'schemaVersion', 'product', 'version', 'build', 'revision', 'previousVersion', 'previousBuild'}
    if set(value) != required or value['schemaVersion'] != 1 or value['product'] != 'xdnote':
        raise ValueError('Invalid release identity fields')
    version, previous = public_version(value['version']), public_version(value['previousVersion'])
    build, previous_build = value['build'], value['previousBuild']
    if (type(build) is not int or type(previous_build) is not int or
            not 0 <= previous_build < build <= 65535 or version <= previous):
        raise ValueError('Version and build must both increase')
    if dirty or configuration != 'Release' or value['revision'] != revision or not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Release identity requires the exact clean Release commit')
    channel = value['version'].split('-')[1].split('.')[0] if '-' in value['version'] else 'stable'
    return {'schemaVersion': 1, 'product': 'xdnote', 'version': value['version'],
            'channel': channel, 'build': build, 'revision': revision, 'dirty': False, 'configuration': configuration}


def identity(root, base, configuration, release=None):
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', base) or any(int(n) > 65535 for n in base.split('.')):
        raise ValueError('Invalid base version')
    root = Path(root).resolve()
    revision, dirty = 'unknown', True
    try:
        def git(*args):
            return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL, text=True, encoding='utf-8').strip()
        if Path(git('rev-parse', '--show-toplevel')).resolve() == root:
            value = git('rev-parse', 'HEAD')
            if re.fullmatch('[0-9a-f]{40}', value):
                revision = value
                dirty = bool(git('status', '--porcelain', '--untracked-files=normal'))
    except (OSError, subprocess.CalledProcessError):
        pass
    if release is not None:
        return release_identity(release, revision, dirty, configuration)
    return {'schemaVersion': 1, 'product': 'xdnote', 'version': base + '-dev.' + revision[:12],
            'channel': 'development', 'build': 0, 'revision': revision,
            'dirty': dirty, 'configuration': configuration}


def write_changed(path, content):
    path = Path(path)
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(content)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--base-version', required=True)
    parser.add_argument('--configuration', required=True)
    parser.add_argument('--release-identity', type=Path, help='explicit external version/build/commit and previous release record; never inferred')
    args = parser.parse_args()
    release = json.loads(args.release_identity.read_text(encoding='utf-8')) if args.release_identity else None
    result = identity(args.source_root, args.base_version, args.configuration, release)
    header = '#pragma once\n' + ''.join(
        '#define XDNOTE_' + key.upper() + ' ' + json.dumps(value) + '\n'
        for key, value in result.items() if key != 'schemaVersion')
    native = result['version'].split('-')[0].replace('.', ',') + ',' + str(result['build'])
    header += '#define XDNOTE_NATIVE_VERSION ' + native + '\n'
    header += '#define XDNOTE_VERSION_FLAGS ' + ('0' if result['channel'] == 'stable' else '2') + '\n'
    write_changed(args.output_dir / 'BuildVersion.h', header.encode())
    write_changed(args.output_dir / 'build-version.json', (json.dumps(result, indent=2) + '\n').encode())


if __name__ == '__main__':
    main()
