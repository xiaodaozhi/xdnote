"""Compile CI-only file fixtures; their .exe contents must never be launched."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from installer_common import ROOT, build_output, digest
from installer_payload import inventory


def main():
    if os.environ.get('CI') != 'true' or os.environ.get('RUNNER_OS') != 'Windows':
        raise RuntimeError('Disposable Windows CI only')
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--toolchain', type=Path, required=True)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    output = build_output(a.output)
    if output.exists():
        raise ValueError('Choose a new fixture output')
    config = json.loads((ROOT / 'scripts/installer-toolchain.json').read_text())['inno_setup']
    for variant in ('1', '2'):
        root = output / ('payload' + variant)
        (root / 'share/notices/Microsoft-VC-runtime').mkdir(parents=True)
        (root / 'xdnote.exe').write_bytes(b'MZ-not-an-executable-QA-' + variant.encode())
        (root / 'qt.conf').write_text('[Paths]\nPrefix=.\n', encoding='utf-8')
        if variant == '1':
            (root / 'retired').mkdir()
            (root / 'retired/old-owned.txt').write_text('owned obsolete fixture', encoding='utf-8')
        notices = root / 'share/notices'
        for item in config['materials']:
            if item['file'] != 'ChineseSimplified.isl':
                shutil.copyfile(a.toolchain / item['file'], notices / item['file'])
        shutil.copyfile(a.runtime / 'Microsoft-VC-runtime-license.rtf', notices / 'Microsoft-VC-runtime/license.rtf')
        (notices / 'Windows-runtime-NOTICE.txt').write_bytes(b'\xef\xbb\xbf' + (ROOT / 'apps/desktop/packaging/windows/Windows-runtime-NOTICE.txt').read_bytes())
        (notices / 'Installer-CREDITS.txt').write_text('RemObjects Pascal Script (https://www.remobjects.com/ps)\n', encoding='utf-8')
        audit = output / ('audit' + variant + '.json')
        audit.write_text(json.dumps({'ok': True, 'qaFixture': True, 'files': inventory(root)}), encoding='utf-8')
        subprocess.run([sys.executable, str(ROOT / 'scripts/build-windows-installer.py'), '--root', str(root),
                        '--audit', str(audit), '--toolchain', str(a.toolchain), '--output', str(output / ('qa' + variant + '.exe')),
                        '--report', str(output / ('qa' + variant + '.json')), '--qa-variant', variant], check=True)


if __name__ == '__main__':
    main()
