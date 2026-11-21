"""Assemble an unsigned six-asset candidate from platform verification receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from installer_common import build_output
from release_candidate import MAX_ASSET_BYTES, TARGETS, validate_identity
from release_preflight import preflight


PLATFORMS = ('windows', 'macos', 'linux')


def unique_fields(items):
    value = dict(items)
    if len(value) != len(items):
        raise ValueError('Duplicate JSON fields')
    return value


def read_record(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('Missing or invalid JSON record: ' + path.name)
    raw = path.read_bytes()
    value = json.loads(raw, object_pairs_hook=unique_fields)
    if not isinstance(value, dict):
        raise ValueError('Expected a JSON object: ' + path.name)
    return value, hashlib.sha256(raw).hexdigest()


def platform_inputs(identity, platform, directory):
    receipt, receipt_digest = read_record(directory / 'verification.json')
    if receipt.get('identity') != identity:
        raise ValueError(f'{platform}: receipt identity differs from the candidate')
    validate_identity(receipt['identity'])
    names = {'xdnote-' + identity['version'] + '-' + suffix
             for system, _, _, suffix, _ in TARGETS if system == platform}
    assets = receipt.get('assets')
    if not isinstance(assets, dict) or set(assets) != names:
        raise ValueError(f'{platform}: receipt must cover exactly its two candidate packages')
    packages = {}
    for name, asset in assets.items():
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing or invalid asset: ' + name)
        if (not isinstance(asset, dict) or asset.get('verified') is not True
                or type(asset.get('bytes')) is not int or not 1 <= asset['bytes'] <= MAX_ASSET_BYTES
                or path.stat().st_size != asset['bytes']):
            raise ValueError('Asset verification missing or stale: ' + name)
        packages[name] = {'bytes': asset['bytes'], 'sha256': asset.get('sha256'), 'verified': asset['verified']}
    return packages, receipt_digest


def assemble(identity, directories, output):
    validate_identity(identity)
    if output.exists() or output.is_symlink() or any(output.is_relative_to(directory) for directory in directories.values()):
        raise ValueError('Use a new output directory outside the input package directories')
    verification = {'identity': identity, 'assets': {}, 'platformReceiptSha256': {}}
    sources = {}
    for platform in PLATFORMS:
        directory = directories[platform]
        assets, receipt_digest = platform_inputs(identity, platform, directory)
        verification['assets'].update(assets)
        verification['platformReceiptSha256'][platform] = receipt_digest
        sources.update((name, directory / name) for name in assets)

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.candidate-', dir=output.parent) as temporary:
        bundle = Path(temporary) / 'bundle'
        assets = bundle / 'assets'
        assets.mkdir(parents=True)
        for name, source in sources.items():
            shutil.copyfile(source, assets / name)
        report = preflight(identity, assets, verification)
        for name, value in (('identity', identity), ('verification', verification), ('preflight', report)):
            (bundle / (name + '.json')).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        if output.exists() or output.is_symlink():
            raise ValueError('Preserve an existing candidate output directory')
        bundle.rename(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('identity', *PLATFORMS, 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        identity, _ = read_record(build_output(args.identity))
        directories = {platform: build_output(getattr(args, platform)) for platform in PLATFORMS}
        result = assemble(identity, directories, build_output(args.output_dir))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps({'identity': result['identity'], 'assets': len(result['candidate']['assets']),
                      'authenticated': result['authenticated'], 'releaseReady': result['releaseReady']}))


if __name__ == '__main__':
    main()
