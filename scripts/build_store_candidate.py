"""Prepare and package an explicit Windows Store candidate in the hosted job."""

import argparse
import json
import os
from pathlib import Path
import re

from background_validation import execute
import build_version
import desktop
from installer_common import ROOT, build_output, digest
import msix_package


def prepare_identity(work):
    work.mkdir(parents=True)
    if not re.fullmatch(r'age1[023456789acdefghjklmnpqrstuvwxyz]{58}', os.environ['XDNOTE_AGE_RECIPIENT']):
        raise ValueError('Supply an age public recipient')
    proposed = {
        'schemaVersion': 1,
        'product': 'xdnote',
        'version': os.environ['XDNOTE_RELEASE_VERSION'],
        'build': int(os.environ['XDNOTE_RELEASE_BUILD']),
        'revision': os.environ['GITHUB_SHA'],
        'previousVersion': os.environ['XDNOTE_PREVIOUS_VERSION'],
        'previousBuild': int(os.environ['XDNOTE_PREVIOUS_BUILD']),
    }
    build_version.identity(ROOT, '0.1.0', 'Release', proposed)
    build_version.write_changed(work / 'release-input.json', json.dumps(proposed, indent=2) + '\n')


def package(work):
    proposed = json.loads((work / 'release-input.json').read_bytes())
    expected = build_version.identity(ROOT, '0.1.0', 'Release', proposed)
    audit = json.loads((work / 'payload-inventory.json').read_bytes())
    if audit['buildIdentity']['buildVersion'] != expected:
        raise ValueError('The sealed payload does not carry the requested release identity')
    recorded = json.loads(os.environ['XDNOTE_MSIX_IDENTITY_JSON'])
    msix_package.manifest(recorded, expected['build'])
    identity_path = work / 'msix-identity.json'
    build_version.write_changed(identity_path, json.dumps(recorded, ensure_ascii=False) + '\n')
    build = desktop.build_path(Path('build/desktop-admission'))
    qt = desktop.checked_tool_path(ROOT / 'build/Qt/6.8.2/msvc2022_64', 'Qt SDK', sdk=True)
    desktop.verify_build_identity(build, 'default', qt)
    assets = work / 'assets'
    msix_package.render_assets(build, qt, assets, work / 'assets.log')
    report = msix_package.build(work / 'payload', audit, recorded, expected['build'], assets, work / 'candidate.msix')
    del report['identity']
    report.update(releaseIdentity=expected, packageVersion=f'1.0.{expected["build"]}.0',
                  identitySource='XDNOTE_MSIX_IDENTITY_JSON', identityInputSha256=digest(identity_path))
    build_version.write_changed(work / 'msix-report.json', json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('identity', 'package'))
    args = parser.parse_args()
    work = build_output(Path('build/store-candidate'))
    if args.operation == 'identity':
        prepare_identity(work)
    else:
        package(work)
