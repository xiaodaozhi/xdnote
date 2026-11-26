"""Restore the authorized signed rc.3 assets from exact Git blobs."""
import argparse
from hashlib import file_digest, sha256
import json
import os
from pathlib import Path
import re
import subprocess


VERIFICATION = 'c9d8196916df0c618888446e0e57f315cd5935a9b9fc68beaa640ca200c3c7b0'
ATTACHMENTS = {'SHA256SUMS', 'appcast.xml', 'macos-update.json', 'release.json', 'release.json.sig'}


def restore(transport, candidate, attachments):
    commit = os.environ['XDNOTE_TRANSFER_COMMIT']
    checksums = os.environ['XDNOTE_CHECKSUMS_SHA256']
    if not re.fullmatch('[0-9a-f]{40}', commit) or not re.fullmatch('[0-9a-f]{64}', checksums):
        raise ValueError('Invalid transfer commit or digest')
    git = ['git', '-C', str(transport)]
    head = subprocess.check_output([*git, 'rev-parse', 'HEAD'], text=True, timeout=15).strip()
    if head != commit:
        raise ValueError('Frozen transfer commit differs')

    def blob(name):
        return subprocess.check_output([*git, 'cat-file', 'blob', f'{commit}:{name}'], timeout=15)

    raw = blob('windows-checksums.json')
    if sha256(raw).hexdigest() != checksums:
        raise ValueError('Transfer checksum manifest differs')
    expected = json.loads(raw)
    verification = blob('verification.json')
    if sha256(verification).hexdigest() != VERIFICATION or expected.get('verification.json') != VERIFICATION:
        raise ValueError('The transfer is not the authorized rc.3 bundle')
    assets = json.loads(verification)['assets']
    targets = {'identity.json': candidate / 'identity.json', 'verification.json': candidate / 'verification.json'}
    for name, receipt in assets.items():
        if not re.fullmatch(r'xdnote-0\.1\.0-rc\.3-[A-Za-z0-9_.-]+', name):
            raise ValueError('Unexpected candidate asset filename')
        relative = 'assets/' + name
        if expected.get(relative) != receipt['sha256']:
            raise ValueError('Asset digest differs from the sealed receipt')
        targets[relative] = candidate / relative
    targets.update({'signed-metadata/' + name: attachments / name for name in ATTACHMENTS})
    if len(assets) != 6 or expected.keys() != targets.keys():
        raise ValueError('Expected exactly the six packages, two identity files, and five signature attachments')
    candidate.mkdir(exist_ok=False)
    attachments.mkdir(exist_ok=False)
    for name, destination in targets.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as output:
            subprocess.run([*git, 'cat-file', 'blob', f'{commit}:{name}'], stdout=output, check=True, timeout=120)
        with destination.open('rb') as stream:
            if file_digest(stream, 'sha256').hexdigest() != expected[name]:
                raise ValueError('Transferred bytes differ: ' + name)
    print('Verified the original bytes of all 13 signed candidate files', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('transport', 'candidate', 'attachments'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    restore(args.transport, args.candidate, args.attachments)
