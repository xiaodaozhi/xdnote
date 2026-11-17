"""Record the actual development source identity; never invent a release build."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def identity(root, base, configuration):
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
    return {'schemaVersion': 1, 'version': base + '-dev.' + revision[:12],
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
    args = parser.parse_args()
    result = identity(args.source_root, args.base_version, args.configuration)
    header = '#pragma once\n' + ''.join(
        '#define XDNOTE_' + key.upper() + ' ' + json.dumps(value) + '\n'
        for key, value in result.items() if key != 'schemaVersion')
    write_changed(args.output_dir / 'BuildVersion.h', header.encode())
    write_changed(args.output_dir / 'build-version.json', (json.dumps(result, indent=2) + '\n').encode())


if __name__ == '__main__':
    main()
