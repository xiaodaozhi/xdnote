"""Prepare a pinned, application-local diagnostic font, never a product asset."""
import argparse
import json
from pathlib import Path
import urllib.request

from installer_common import ROOT, build_output, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    output = build_output(a.output); output.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'scripts/ci-font-materials.json').read_text())
    for record in config['files']:
        target = output / record['file']
        if not target.exists():
            with urllib.request.urlopen(record['url'], timeout=120) as response:
                target.write_bytes(response.read())
        if digest(target) != record['sha256']:
            raise ValueError('Pinned diagnostic font/material hash mismatch')
    (output / 'receipt.json').write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
    print('Diagnostic font and original license verified; no system installation or product bundling')


if __name__ == '__main__':
    main()
