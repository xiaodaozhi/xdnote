"""Prepare an isolated macOS VoiceOver fixture or check its unchanged sample."""

import argparse
import json
from pathlib import Path
import platform
import plistlib
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from installer_common import build_output, digest

APP = 'xdnote.app'
PRODUCT_ID = 'io.github.xiaodaozhi.xdnote'
SAMPLE = 'VoiceOver manual.md'
PROGRAM = 'Contents/MacOS/xdnote'


def sample_text():
    return (
        '# VoiceOver opening\n\n[Jump to Amber](#distant-amber-heading)\n\n'
        + ''.join(f'Paragraph {i}.\n\n' for i in range(1, 36))
        + '## Distant Amber Heading\n\n'
        'Amber target reading sample continues here. The amber lantern lights the quiet path.\n\n'
        'The sentence after the jump is readable from start to finish.\n'
    )


def tool(name, *args):
    subprocess.run(['/usr/bin/' + name, *map(str, args)], check=True, timeout=120)


def prepare(source, output):
    if (platform.system(), platform.machine()) != ('Darwin', 'arm64'):
        raise ValueError('Use an Apple Silicon Mac with a sealed xdnote product bundle')
    if source.name != APP or output.exists() or output.is_relative_to(source):
        raise ValueError('Use a sealed xdnote.app and a new fixture directory outside it')
    tool('codesign', '--verify', '--deep', '--strict', source)
    original_hash = digest(source / PROGRAM)
    info = plistlib.loads((source / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != PRODUCT_ID:
        raise ValueError('Expected a standard deployed xdnote product')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.voiceover-fixture-', dir=output.parent) as temporary:
        pending = Path(temporary) / 'fixture'
        pending.mkdir()
        app = pending / APP
        tool('ditto', source, app)
        (pending / 'home').mkdir()
        info.update(CFBundleIdentifier=PRODUCT_ID + '.voiceover.fixture.' + uuid.uuid4().hex,
                    CFBundleDisplayName='xdnote VoiceOver Test',
                    LSEnvironment={'CFFIXED_USER_HOME': str(output / 'home')})
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
        tool('codesign', '--force', '--sign', '-', app)
        tool('codesign', '--verify', '--deep', '--strict', app)
        sample = pending / SAMPLE
        sample.write_text(sample_text(), encoding='utf-8')
        receipt = {'version': info['CFBundleShortVersionString'], 'sourceExecutableSha256': original_hash,
                   'fixtureExecutableSha256': digest(app / PROGRAM), 'sampleSha256': digest(sample)}
        (pending / 'fixture.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        if output.exists() or output.is_symlink():
            raise ValueError('Preserve the existing fixture directory')
        pending.rename(output)
    print(f'App: {output / APP}\nSample: {output / SAMPLE}')


def check(output):
    receipt = json.loads((output / 'fixture.json').read_bytes())
    if digest(output / SAMPLE) != receipt['sampleSha256']:
        raise ValueError('The Markdown sample changed during acceptance')
    print('Markdown sample SHA-256: ' + receipt['sampleSha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    setup = actions.add_parser('prepare', help='Copy the sealed app and create a Markdown fixture')
    setup.add_argument('--source-app', required=True, type=Path)
    setup.add_argument('--work-dir', required=True, type=Path)
    verify = actions.add_parser('check', help='Compare the tested sample with its original SHA-256')
    verify.add_argument('--work-dir', required=True, type=Path)
    args = parser.parse_args()
    try:
        work = build_output(args.work_dir)
        if args.action == 'prepare':
            prepare(build_output(args.source_app), work)
        else:
            check(work)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
