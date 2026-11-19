"""Assemble and independently verify update attachments for one frozen release candidate."""

import argparse
import base64
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib
import xml.etree.ElementTree as ET

import release_candidate
from installer_common import ROOT, build_output, digest


KEY_ID = 'XDNOTE_UPDATE_KEY_ID'
MANIFEST_KEY = 'XDNOTE_UPDATE_SIGNING_KEY_HEX'
SPARKLE_KEY = 'XDNOTE_SPARKLE_SIGNING_KEY_HEX'
SIGNING_VARIABLES = (KEY_ID, MANIFEST_KEY, SPARKLE_KEY)
ATTACHMENTS = ('release.json', 'release.json.sig', 'appcast.xml', 'macos-update.json')
SPARKLE_NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
ET.register_namespace('sparkle', SPARKLE_NS)


def bounded(path, limit):
    if not path.is_file() or path.is_symlink():
        raise ValueError(f'Missing or invalid input file: {path.name}')
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f'Input exceeds the size limit: {path.name}')
    return data


def read_keys(path):
    try:
        text = bounded(path, 4096).decode('utf-8-sig')
    except UnicodeDecodeError:
        raise ValueError('Signing key TXT must be UTF-8') from None
    fields = {}
    for row in text.splitlines():
        row = row.strip()
        if not row or row.startswith('#'):
            continue
        name, separator, value = row.partition('=')
        if not separator or name not in SIGNING_VARIABLES or name in fields:
            raise ValueError('Unexpected or repeated field in signing key TXT')
        fields[name] = value.strip()
    return fields


def signing_inputs(path):
    fields = read_keys(path) if path else {name: os.environ.get(name, '') for name in SIGNING_VARIABLES}
    if set(fields) != set(SIGNING_VARIABLES) or not all(fields.values()):
        raise ValueError('Supply a key ID and both release signing keys')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', fields[KEY_ID]):
        raise ValueError('Invalid manifest key ID')
    seeds = fields[MANIFEST_KEY], fields[SPARKLE_KEY]
    if not all(re.fullmatch(r'[0-9a-f]{64}', seed) for seed in seeds) or seeds[0] == seeds[1]:
        raise ValueError('Supply two distinct 32-byte Ed25519 signing keys as lowercase hex')
    return fields


def public_environment():
    return {name: value for name, value in os.environ.items() if name not in SIGNING_VARIABLES}


def run_tool(tool, operation, *arguments, credentials=None):
    environment = public_environment()
    if credentials:
        environment.update(credentials)
    result = subprocess.run([tool, operation, *arguments], env=environment, capture_output=True)
    if result.returncode:
        raise ValueError(f'Release signing tool: {operation} failed')


def build_tool():
    environment = public_environment()
    rustup = shutil.which('rustup', path=environment.get('PATH'))
    if not rustup:
        raise ValueError('Prepare the project Rust toolchain, or provide the built signing tool')
    toolchain = tomllib.loads((ROOT / 'rust-toolchain.toml').read_text(encoding='utf-8'))['toolchain']['channel']
    location = subprocess.run([rustup, 'which', '--toolchain', toolchain, 'cargo'], env=environment, capture_output=True, text=True)
    if location.returncode:
        raise ValueError('Prepare the pinned Rust toolchain before signing')
    cargo = Path(location.stdout.strip())
    environment['PATH'] = str(cargo.parent) + os.pathsep + environment.get('PATH', '')
    environment['CARGO_HOME'] = str(ROOT / 'build/cargo-home')
    environment['CARGO_TARGET_DIR'] = str(ROOT / 'build/release-tool')
    result = subprocess.run([cargo, 'build', '--locked', '--offline', '--release', '-p', 'xdnote-update',
                             '--features', 'release-tool', '--bin', 'xdnote-update-tool'],
                            cwd=ROOT, env=environment, capture_output=True)
    if result.returncode:
        raise ValueError('Build the signing tool with the locked Rust dependencies before signing')
    binary = 'xdnote-update-tool.exe' if os.name == 'nt' else 'xdnote-update-tool'
    return ROOT / 'build/release-tool/release' / binary


def appcast_bytes(candidate, archive, signature):
    rss = ET.Element('rss', version='2.0')
    channel = ET.SubElement(rss, 'channel')
    ET.SubElement(channel, 'title').text = 'xdnote'
    item = ET.SubElement(channel, 'item')
    ET.SubElement(item, 'title').text = f"xdnote {candidate['version']}"
    ET.SubElement(item, f'{{{SPARKLE_NS}}}version').text = str(candidate['build'])
    ET.SubElement(item, f'{{{SPARKLE_NS}}}shortVersionString').text = candidate['version']
    ET.SubElement(item, f'{{{SPARKLE_NS}}}minimumSystemVersion').text = archive['minimum_system']['macos']
    if candidate['channel'] != 'stable':
        ET.SubElement(item, f'{{{SPARKLE_NS}}}channel').text = candidate['channel']
    ET.SubElement(item, 'enclosure', {'url': archive['url'], 'length': str(archive['bytes']),
                  'type': 'application/octet-stream', f'{{{SPARKLE_NS}}}edSignature': signature})
    return ET.tostring(rss, encoding='utf-8', xml_declaration=True) + b'\n'


def sparkle_archive(candidate):
    return next(asset for asset in candidate['assets'] if asset['platform'] == 'macos' and asset['package_type'] == 'app.zip')


def archive_signature(candidate, archive, receipt):
    expected = {'schema_version': 1, 'version': candidate['version'], 'build': candidate['build'],
                **{name: archive[name] for name in ('filename', 'bytes', 'sha256')}}
    if set(receipt) != set(expected) | {'ed_signature'} or any(
        type(receipt[name]) is not type(value) or receipt[name] != value for name, value in expected.items()
    ):
        raise ValueError('Sparkle receipt differs from the frozen archive and candidate')
    signature = receipt['ed_signature']
    if len(base64.b64decode(signature, validate=True)) != 64:
        raise ValueError('Invalid Sparkle archive signature')
    return signature


def checksum_bytes(candidate, output):
    hashes = {asset['filename']: asset['sha256'] for asset in candidate['assets']}
    hashes.update({name: digest(output / name) for name in ATTACHMENTS})
    return ''.join(f'{sha}  {name}\n' for name, sha in sorted(hashes.items())).encode('ascii')


def verify_attachments(candidate, assets, output, tool):
    archive = sparkle_archive(candidate)
    signature = archive_signature(candidate, archive, json.loads(bounded(output / 'macos-update.json', 4096)))
    manifest = output / 'release.json'
    envelope = output / 'release.json.sig'
    feed = output / 'appcast.xml'
    if json.loads(bounded(manifest, 256 * 1024)) != candidate:
        raise ValueError('Signed manifest differs from the verified candidate')
    if not bounded(feed, 256 * 1024 + 512).startswith(appcast_bytes(candidate, archive, signature) + b'<!-- sparkle-signatures:\n'):
        raise ValueError('Signed appcast differs from the verified candidate')
    run_tool(tool, 'verify-archive', assets / archive['filename'], signature)
    run_tool(tool, 'verify-appcast', feed)
    run_tool(tool, 'verify', manifest, envelope)
    if bounded(output / 'SHA256SUMS', 8192) != checksum_bytes(candidate, output):
        raise ValueError('SHA256SUMS differs from the verified candidate and attachments')


def verify_release(identity, assets, verification, output, tool):
    if {item.name for item in output.iterdir()} != {*ATTACHMENTS, 'SHA256SUMS'}:
        raise ValueError('Signed metadata directory must contain exactly the release attachments and SHA256SUMS')
    verify_attachments(release_candidate.manifest(identity, assets, verification), assets, output, tool)


def sign_release(identity, assets, verification, keys, output, *, tool):
    if output.exists() or output.is_symlink():
        raise ValueError('Preserve the existing output directory')
    candidate = release_candidate.manifest(identity, assets, verification)
    fields = signing_inputs(keys)
    run_tool(tool, 'check-signing-keys', credentials=fields)
    archive = sparkle_archive(candidate)
    sparkle = {SPARKLE_KEY: fields[SPARKLE_KEY]}

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sign-release-', dir=output.parent) as directory:
        temporary = Path(directory)
        stage = temporary / 'metadata'
        stage.mkdir()
        receipt = stage / 'macos-update.json'
        run_tool(tool, 'sign-archive', assets / archive['filename'], candidate['version'],
                 str(candidate['build']), receipt, credentials=sparkle)
        signature = archive_signature(candidate, archive, json.loads(bounded(receipt, 4096)))
        unsigned_feed = temporary / 'appcast.xml'
        unsigned_feed.write_bytes(appcast_bytes(candidate, archive, signature))
        run_tool(tool, 'sign-appcast', unsigned_feed, stage / 'appcast.xml', credentials=sparkle)

        manifest = stage / 'release.json'
        manifest.write_text(json.dumps(candidate, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        run_tool(tool, 'sign', manifest, stage / 'release.json.sig',
                 credentials={KEY_ID: fields[KEY_ID], MANIFEST_KEY: fields[MANIFEST_KEY]})
        (stage / 'SHA256SUMS').write_bytes(checksum_bytes(candidate, stage))
        verify_release(identity, assets, verification, stage, tool)
        if output.exists() or output.is_symlink():
            raise ValueError('Preserve the existing output directory')
        stage.rename(output)


def main(*, offline=False):
    parser = argparse.ArgumentParser(description=__doc__)
    if not offline:
        parser.add_argument('operation', choices=('sign', 'verify'))
    for name in ('identity', 'assets', 'verification', 'output', 'tool'):
        parser.add_argument(f'--{name}', type=Path, required=name != 'tool')
    parser.add_argument('--keys', type=Path, required=offline, help='Optional offline signing TXT')
    parser.add_argument('--revision', help='Require this frozen source revision in the candidate evidence')
    args = parser.parse_args()
    operation = 'sign' if offline else args.operation
    if operation == 'verify' and args.keys is not None:
        parser.error('Public verification uses the compiled public trust roots')
    try:
        identity = json.loads(bounded(args.identity, 16384))
        if args.revision and identity.get('revision') != args.revision:
            raise ValueError('Candidate revision differs from the frozen source revision')
        verification = json.loads(bounded(args.verification, 64 * 1024))
        assets = build_output(args.assets)
        output = build_output(args.output)
        tool = build_output(args.tool) if args.tool else build_tool()
        if operation == 'sign':
            sign_release(identity, assets, verification, args.keys, output, tool=tool)
        else:
            verify_release(identity, assets, verification, output, tool)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print('Release update attachments verified. Native acceptance and publication remain separate.')


if __name__ == '__main__':
    main()
