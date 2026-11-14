#!/usr/bin/env python3
"""Extract pinned Microsoft redistributables without installing an MSI or EXE."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile
from installer_common import ROOT, digest, within, build_output


def fetch(record, path):
    if not path.exists():
        with urllib.request.urlopen(record['url'], timeout=120) as response:
            path.write_bytes(response.read())
    if digest(path) != record['sha256']:
        raise ValueError('Pinned runtime/tool download hash mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = build_output(args.output)
    config = json.loads((ROOT/'scripts/windows-runtime.json').read_text())
    output.mkdir(parents=True, exist_ok=True)
    fetch(config, output/'vc_redist.x64.exe')
    fetch(config['wix'], output/'wix.zip')
    tools = output/'wix'
    with zipfile.ZipFile(output/'wix.zip') as archive:
        if any(not within(tools/item.filename, tools) for item in archive.infolist()):
            raise ValueError('Unbounded tool archive')
        archive.extractall(tools)
    # Only hash-pinned build tools and supplier bytes are unpacked. Neither the
    # redistributable bootstrapper nor its MSI packages are executed/installed.
    extracted = output/'extracted'
    subprocess.run([str(tools/'dark.exe'), '-nologo', '-x', str(extracted),
                    str(output/'vc_redist.x64.exe')], check=True, stdout=subprocess.DEVNULL)
    raw = output/'raw'; raw.mkdir(exist_ok=True)
    cab = extracted/'AttachedContainer/packages/vcRuntimeMinimum_amd64/cab1.cab'
    subprocess.run(['expand.exe', '-F:*', str(cab), str(raw)], check=True, stdout=subprocess.DEVNULL)
    runtime = output/'runtime'; runtime.mkdir(exist_ok=True)
    for name, expected in config['files'].items():
        path = raw/(name+'_amd64')
        if digest(path) != expected:
            raise ValueError('Extracted CRT hash mismatch')
        shutil.copyfile(path, runtime/name)
    license_file = extracted/'UX/license.rtf'
    if digest(license_file) != config['license_sha256']:
        raise ValueError('Runtime license hash mismatch')
    shutil.copyfile(license_file, runtime/'Microsoft-VC-runtime-license.rtf')
    print('Prepared pinned x64 app-local VC++ runtime, no system installation')


if __name__ == '__main__':
    main()
