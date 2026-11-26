"""Fetch and bind the exact anonymous GitHub Release and approved assets."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


TAG = 'v0.1.0-rc.3'
REPOSITORY = 'xiaodaozhi/xdnote'
REVISION = '10fcb69062df2fea383e31fb83c08907929b51cb'
BASE = f'https://github.com/{REPOSITORY}/releases/download/{TAG}/'
SUMS_SHA256 = '7536e8cf839352ad75ae6137e694e527a35207886da69ae1632e4e20fc11aad4'
ATTACHMENTS = {'release.json', 'release.json.sig', 'macos-update.json', 'appcast.xml'}


class TrustedRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        url = urlsplit(newurl)
        if (url.scheme != 'https' or url.hostname not in {'github.com', 'api.github.com',
                'objects.githubusercontent.com', 'release-assets.githubusercontent.com'} or url.username):
            raise ValueError('Unexpected release redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


opener = build_opener(TrustedRedirects())


def get(url, limit):
    req = Request(url, headers={'User-Agent': 'xdnote-rc3-public-release-acceptance'})
    with opener.open(req, timeout=45) as response:
        data = response.read(limit + 1)
        if response.status != 200 or len(data) > limit:
            raise ValueError('Public resource size or status differs')
        return data, urlsplit(response.url).hostname


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=False)
    metadata = out / 'signed-metadata'
    assets = out / 'release-verified/assets'
    assets.mkdir(parents=True)
    metadata.mkdir()
    listing_bytes, _ = get(f'https://api.github.com/repos/{REPOSITORY}/releases?per_page=100&page=1', 8 * 1024**2)
    listing = json.loads(listing_bytes)
    release, = [row for row in listing if row['tag_name'] == TAG and not row['draft']]
    if not release['prerelease']:
        raise ValueError('Expected the rc.3 prerelease channel')
    ref_bytes, _ = get(f'https://api.github.com/repos/{REPOSITORY}/git/ref/tags/{TAG}', 16384)
    ref = json.loads(ref_bytes)['object']
    if ref != {'type': 'commit', 'sha': REVISION, 'url': f'https://api.github.com/repos/{REPOSITORY}/git/commits/{REVISION}'}:
        raise ValueError('Published source tag does not identify the authorized revision')
    sums, host = get(BASE + 'SHA256SUMS', 8192)
    if sha256(sums).hexdigest() != SUMS_SHA256:
        raise ValueError('Published checksum file differs from the authorized signature output')
    (metadata / 'SHA256SUMS').write_bytes(sums)
    expected = {}
    for line in sums.decode('ascii').splitlines():
        digest, name = line.split(maxsplit=1)
        if name in expected or not re.fullmatch(r'[a-zA-Z0-9_.-]+', name) or not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError('Unexpected checksum entry')
        expected[name] = digest
    public_assets = {row['name']: row for row in release['assets']}
    if set(public_assets) != expected.keys() | {'SHA256SUMS'} or len(expected) != 10:
        raise ValueError('Public asset set differs')
    receipts = {}
    for name, digest in expected.items():
        listed = public_assets[name]
        if listed['browser_download_url'] != BASE + name:
            raise ValueError('Unexpected canonical release download URL')
        data, final_host = get(BASE + name, min(listed['size'], 256 * 1024**2))
        if len(data) != listed['size'] or sha256(data).hexdigest() != digest:
            raise ValueError('Download bytes differ: ' + name)
        location = metadata if name in ATTACHMENTS else assets
        (location / name).write_bytes(data)
        receipts[name] = {'bytes': len(data), 'sha256': digest, 'verified': True, 'downloadHost': final_host}
    manifest = json.loads((metadata / 'release.json').read_bytes())
    if manifest['build'] != 305 or manifest['commit'] != REVISION or manifest['tag'] != TAG:
        raise ValueError('Public manifest identity differs')
    identity = {'schemaVersion': 1, 'product': 'xdnote', 'version': '0.1.0-rc.3', 'channel': 'rc',
                'build': 305, 'revision': REVISION, 'dirty': False, 'configuration': 'Release'}
    verification = {'identity': identity, 'assets': {n: {k: v for k, v in r.items() if k != 'downloadHost'}
                   for n, r in receipts.items() if n not in ATTACHMENTS}}
    (assets.parent/'identity.json').write_text(json.dumps(identity), encoding='utf-8')
    (assets.parent/'verification.json').write_text(json.dumps(verification), encoding='utf-8')
    (out/'public-delivery.json').write_text(json.dumps({'tag': TAG, 'release': release['html_url'],
        'revision': REVISION, 'sha256sumsHost': host, 'sha256sums': SUMS_SHA256, 'assets': receipts}, indent=2), encoding='utf-8')
    print(f'Public Release listing, target and all {len(public_assets)} assets verified')


if __name__ == '__main__':
    main()
