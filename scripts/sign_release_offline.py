"""Create and verify local release.json, its signature, and a signed Sparkle appcast."""

import argparse
import base64
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import release_candidate
import sparkle
from installer_common import ROOT, build_output, digest


BACKUP_FIELDS = {
    'XDNOTE_UPDATE_KEY_ID',
    'XDNOTE_UPDATE_SIGNING_KEY_HEX',
    'XDNOTE_SPARKLE_SIGNING_KEY_HEX',
}
PRIVATE_DER = bytes.fromhex('302e020100300506032b657004220420')
PUBLIC_DER = bytes.fromhex('302a300506032b6570032100')
SPARKLE_NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
ET.register_namespace('sparkle', SPARKLE_NS)


def read_keys(path):
    with path.open('rb') as stream:
        raw = stream.read(4097)
    if len(raw) > 4096:
        raise ValueError('Signing key TXT exceeds the size limit')
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        raise ValueError('Signing key TXT must be UTF-8') from None
    fields = {}
    for row in text.splitlines():
        row = row.strip()
        if not row or row.startswith('#'):
            continue
        name, separator, value = row.partition('=')
        if not separator or name not in BACKUP_FIELDS or name in fields:
            raise ValueError('Unexpected or repeated field in signing key TXT')
        fields[name] = value.strip()
    if set(fields) != BACKUP_FIELDS:
        raise ValueError('Signing key TXT is missing required fields')
    key_id = fields['XDNOTE_UPDATE_KEY_ID']
    manifest = fields['XDNOTE_UPDATE_SIGNING_KEY_HEX']
    sparkle = fields['XDNOTE_SPARKLE_SIGNING_KEY_HEX']
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', key_id):
        raise ValueError('Invalid manifest key ID')
    if not all(re.fullmatch(r'[0-9a-f]{64}', value) for value in (manifest, sparkle)) or manifest == sparkle:
        raise ValueError('Supply two distinct 32-byte Ed25519 signing keys as lowercase hex')
    return key_id, bytes.fromhex(manifest), bytes.fromhex(sparkle)


def checked(arguments, operation, *, data):
    # Tool diagnostics can contain input. Only report the public operation name.
    result = subprocess.run(arguments, input=data, capture_output=True)
    if result.returncode:
        raise ValueError(f'{operation} failed')
    return result.stdout


def verify_signing_keys(openssl, key_id, manifest_seed, sparkle_seed):
    roots = json.loads((ROOT / 'tools/update-trust.json').read_bytes())['keys']
    matching = [bytes.fromhex(key['public_key']) for key in roots if key['key_id'] == key_id]
    if len(matching) != 1:
        raise ValueError('Manifest key ID does not match the configured public trust root')
    sparkle = json.loads((ROOT / 'tools/sparkle-update-key.json').read_bytes())['publicKey']
    expected_sparkle = base64.b64decode(sparkle, validate=True)
    for name, seed, public in [('Manifest', manifest_seed, matching[0]), ('Sparkle', sparkle_seed, expected_sparkle)]:
        derived = checked([openssl, 'pkey', '-inform', 'DER', '-pubout', '-outform', 'DER'],
                          f'{name} public key derivation', data=PRIVATE_DER + seed)
        if derived != PUBLIC_DER + public:
            raise ValueError(f'{name} signing key does not match the configured public key')


def sparkle_command(tool, seed, operation, *arguments):
    return checked([tool, '--ed-key-file', '-', *arguments], operation,
                   data=base64.b64encode(seed) + b'\n')


def sign_file(tool, seed, path):
    signature = sparkle_command(tool, seed, 'Ed25519 signing', '-p', path).strip()
    if len(base64.b64decode(signature, validate=True)) != 64:
        raise ValueError('Unexpected Ed25519 signature length')
    encoded = signature.decode('ascii')
    sparkle_command(tool, seed, 'Ed25519 verification', '--verify', path, encoded)
    return encoded


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


def sign_release(identity, assets, verification, keys, output, *, tool, openssl):
    if output.exists() or output.is_symlink():
        raise ValueError('Preserve the existing output directory')
    candidate = release_candidate.manifest(identity, assets, verification)
    archive = next(asset for asset in candidate['assets'] if asset['platform'] == 'macos' and asset['package_type'] == 'app.zip')
    archive_path = assets / archive['filename']
    key_id, manifest_seed, sparkle_seed = read_keys(keys)
    verify_signing_keys(openssl, key_id, manifest_seed, sparkle_seed)

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sign-release-', dir=output.parent) as directory:
        stage = Path(directory) / 'metadata'
        stage.mkdir()
        signature = sign_file(tool, sparkle_seed, archive_path)
        if digest(archive_path) != archive['sha256']:
            raise ValueError('The Sparkle archive changed during signing')
        appcast = stage / 'appcast.xml'
        appcast.write_bytes(appcast_bytes(candidate, archive, signature))
        sparkle_command(tool, sparkle_seed, 'Appcast signing', '--disable-signing-warning', appcast)
        sparkle_command(tool, sparkle_seed, 'Appcast verification', '--verify', appcast)

        manifest = stage / 'release.json'
        unsigned = (json.dumps(candidate, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
        manifest.write_bytes(unsigned)
        signature = sign_file(tool, manifest_seed, manifest)
        if manifest.read_bytes() != unsigned:
            raise ValueError('The original manifest changed during signing')
        envelope = {'schema_version': 1, 'algorithm': 'ed25519', 'key_id': key_id,
                    'signature': base64.b64decode(signature, validate=True).hex()}
        (stage / 'release.json.sig').write_text(json.dumps(envelope) + '\n', encoding='utf-8')
        if output.exists() or output.is_symlink():
            raise ValueError('Preserve the existing output directory')
        stage.rename(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('keys', 'identity', 'assets', 'verification', 'output'):
        parser.add_argument(f'--{name}', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('The offline release signer uses the pinned macOS Sparkle SDK')
    try:
        openssl = shutil.which('openssl')
        if not openssl:
            raise ValueError('OpenSSL with Ed25519 support is required')
        upstream = sparkle.BASE / 'upstream'
        sparkle.verify(upstream)
        tool = upstream / 'sdk/bin/sign_update'
        sign_release(json.loads(args.identity.read_bytes()), build_output(args.assets),
                     json.loads(args.verification.read_bytes()), args.keys, build_output(args.output),
                     tool=tool, openssl=openssl)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print('Local release metadata signed and verified. Release acceptance is separate.')


if __name__ == '__main__':
    main()
