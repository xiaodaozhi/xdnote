"""Explicit preparation and native library admission; never enables product updates."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import shutil
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'build/Sparkle/2.10.0'
RECEIPT = 'xdnote-sdk.json'


def configuration():
    value = json.loads((ROOT / 'tools/sparkle-dependency.json').read_text(encoding='utf-8'))
    if value['schemaVersion'] != 1 or value['version'] != '2.10.0':
        raise ValueError('Unsupported Sparkle dependency')
    if digest(ROOT / value['licenseFile']) != value['license_sha256']:
        raise ValueError('Recorded Sparkle license changed')
    return value


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        while block := source.read(1024 * 1024):
            result.update(block)
    return result.hexdigest()


def owned(path):
    """All inputs, SDKs and generated output belong to this project's build/."""
    path = Path(os.path.abspath(path))
    boundary = ROOT / 'build'
    if not path.is_relative_to(boundary) or path == boundary:
        raise ValueError('Sparkle path must be below project build/')
    for item in (path, *path.parents):
        if item == ROOT:
            break
        if item.is_symlink() or (hasattr(item, 'is_junction') and item.is_junction()):
            raise ValueError('Sparkle path contains a link')
    return path


def inventory(directory):
    root = Path(directory).resolve()
    result = {}
    for path in sorted(root.rglob('*')):
        if path.name == RECEIPT and path.parent == root:
            continue
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(root):
                raise ValueError('Sparkle link escapes its SDK')
            result[relative] = {'link': os.readlink(path)}
        elif path.is_file():
            result[relative] = {'sha256': digest(path), 'mode': path.stat().st_mode & 0o777}
        elif not path.is_dir():
            raise ValueError('Sparkle tree contains a special file')
    return result


def extract(archive, output, *, links=False):
    """Pinned input still gets bounded member, path, type and symlink checks."""
    root = Path(output).resolve()
    root.mkdir(parents=True)
    with tarfile.open(archive) as source:
        members = source.getmembers()
        if len(members) > 20000 or sum(max(0, m.size) for m in members) > 512 * 1024 * 1024:
            raise ValueError('Sparkle archive exceeds extraction limits')
        names, symlinks = set(), set()
        for member in members:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts or '\\' in member.name:
                raise ValueError('Unsafe Sparkle archive path')
            if str(name) == '.':
                continue
            if name in names or member.size > 64 * 1024 * 1024:
                raise ValueError('Duplicate or oversized Sparkle member')
            if not (member.isfile() or member.isdir() or (links and member.issym())):
                raise ValueError('Unsupported Sparkle archive member')
            names.add(name)
            if member.issym():
                symlinks.add(name)
                target = root / name.parent / member.linkname
                if Path(member.linkname).is_absolute() or not target.resolve().is_relative_to(root):
                    raise ValueError('Sparkle archive link escapes output')
        if any(parent in symlinks for name in names for parent in name.parents):
            raise ValueError('Sparkle member traverses an archive link')
        for member in members:
            name = PurePosixPath(member.name)
            if str(name) == '.':
                continue
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.isdir():
                target.mkdir(exist_ok=True)
            elif member.issym():
                target.symlink_to(member.linkname)
            else:
                with source.extractfile(member) as data, target.open('xb') as destination:
                    shutil.copyfileobj(data, destination)
                target.chmod(member.mode & 0o777)
    inventory(root)  # Resolve all link chains only after the whole tree exists.


def material(spec, download):
    target = owned(ROOT / 'build/source-cache' / spec['file'])
    if not target.exists():
        if not download:
            raise ValueError('Sparkle material missing; explicitly prepare with --download')
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as partial:
            temporary = Path(partial.name)
            try:
                request = urllib.request.Request(spec['url'], headers={'User-Agent': 'xdnote-build/1'})
                started, count = time.monotonic(), 0
                with urllib.request.urlopen(request, timeout=90) as response:
                    if not response.url.startswith('https://'):
                        raise ValueError('Sparkle material redirected outside HTTPS')
                    while block := response.read(1024 * 1024):
                        count += len(block)
                        if count > 64 * 1024 * 1024 or time.monotonic() - started > 180:
                            raise ValueError('Sparkle download exceeds bounds')
                        partial.write(block)
                partial.flush()
                if digest(temporary) != spec['sha256']:
                    raise ValueError('Sparkle archive checksum differs')
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
    if not target.is_file() or target.stat().st_size > 64 * 1024 * 1024 or digest(target) != spec['sha256']:
        raise ValueError('Cached Sparkle archive checksum differs')
    return target


def verify(directory, *, built=False):
    directory = owned(directory)
    receipt_path = directory / RECEIPT
    if receipt_path.is_symlink():
        raise ValueError('Sparkle receipt is a link')
    receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
    spec = configuration()
    if receipt.get('dependency') != spec or inventory(directory) != receipt.get('files'):
        raise ValueError('Sparkle SDK identity differs')
    sdk = directory / 'sdk' if not built else directory
    required = ('LICENSE', 'Sparkle.framework/Versions/B/Sparkle',
                'Sparkle.framework/Versions/B/Headers/SPUUpdater.h',
                'Sparkle.framework/Versions/B/Autoupdate',
                'Sparkle.framework/Versions/B/Updater.app/Contents/MacOS/Updater',
                'Sparkle.framework/Versions/B/XPCServices/Downloader.xpc/Contents/MacOS/Downloader',
                'Sparkle.framework/Versions/B/XPCServices/Installer.xpc/Contents/MacOS/Installer')
    if any((sdk / name).is_symlink() or not (sdk / name).is_file() for name in required):
        raise ValueError('Sparkle SDK is incomplete')
    if digest(sdk / 'LICENSE') != spec['license_sha256']:
        raise ValueError('Sparkle SDK license differs')
    info = plistlib.loads((sdk / 'Sparkle.framework/Versions/B/Resources/Info.plist').read_bytes())
    if info.get('CFBundleShortVersionString') != spec['version']:
        raise ValueError('Sparkle framework version differs')
    if built:
        import sparkle_build
        expected = sparkle_build.inputs(spec)
        record = receipt.get('frameworkBuild', {})
        if any(record.get(key) != value for key, value in expected.items()):
            raise ValueError('Sparkle native build inputs differ')
        if info.get('XdnoteFrameworkBuildVariant') != expected['variant']:
            raise ValueError('Sparkle proxy variant differs')
    elif receipt.get('frameworkBuild') is not None:
        raise ValueError('Expected original Sparkle inputs')
    return receipt


def prepare(download=False):
    output = owned(BASE / 'upstream')
    spec = configuration()
    if output.exists():
        verify(output)
        return output
    sdk, source = material(spec['sdk'], download), material(spec['source'], download)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-', dir=output.parent) as name:
        stage = Path(name)
        extract(sdk, stage / 'sdk', links=True)
        extract(source, stage / 'source')
        if digest(stage / 'sdk/LICENSE') != spec['license_sha256']:
            raise ValueError('Upstream Sparkle license differs')
        receipt = {'dependency': spec, 'files': inventory(stage)}
        (stage / RECEIPT).write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        verify(stage)
        stage.rename(output)
    return output


def build():
    import sparkle_build
    if platform.system() != 'Darwin' or platform.machine() not in ('arm64', 'x86_64'):
        raise ValueError('Sparkle admission requires native macOS Intel or ARM64')
    upstream = owned(BASE / 'upstream')
    verify(upstream)
    output = owned(BASE / platform.machine())
    if output.exists():
        verify(output, built=True)
        return output
    spec = configuration()
    with tempfile.TemporaryDirectory(prefix='compile-', dir=BASE) as name:
        stage = Path(name)
        shutil.copytree(upstream / 'sdk', stage / 'sdk', symlinks=True)
        sources = list((upstream / 'source').iterdir())
        if len(sources) != 1 or sources[0].is_symlink() or not sources[0].is_dir():
            raise ValueError('Unexpected Sparkle source root')
        shutil.copytree(sources[0], stage / 'source')
        record = sparkle_build.build(stage / 'source', stage / 'sdk', stage / 'objects', spec,
                                     BASE / (platform.machine() + '-build.log'))
        original = inventory(upstream / 'sdk')
        files = inventory(stage / 'sdk')
        changed = {'Sparkle.framework/Versions/B/' + path for path in (
            'Sparkle', 'Headers/SPUUpdater.h', 'Resources/Info.plist', '_CodeSignature/CodeResources')}
        for relative, identity in original.items():
            if relative not in changed and not relative.startswith('Symbols/') and files.get(relative) != identity:
                raise ValueError('Unexplained Sparkle helper/resource/tool mutation: ' + relative)
        if set(files) - set(original):
            raise ValueError('Unexplained Sparkle SDK addition')
        receipt = {'dependency': spec, 'frameworkBuild': record, 'files': files}
        (stage / 'sdk' / RECEIPT).write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        verify(stage / 'sdk', built=True)
        (stage / 'sdk').rename(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build', 'verify'))
    parser.add_argument('--download', action='store_true')
    args = parser.parse_args()
    if args.download and args.command != 'prepare':
        parser.error('--download is supported by prepare only')
    if args.command == 'prepare':
        result = prepare(args.download)
    elif args.command == 'build':
        result = build()
    else:
        result = owned(BASE / platform.machine())
        verify(result, built=True)
    print(result.relative_to(ROOT))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error).replace(str(ROOT), '<workspace>'))
