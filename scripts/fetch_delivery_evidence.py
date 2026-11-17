"""Fetch bounded successful product evidence from this repository's Actions.

Uses the CI GitHub token through gh; no release, tag or repository writes.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import zipfile
from installer_common import build_output

REPO = 'xiaodaozhi/xdnote'
HOSTS = {'windows': 'windows-2022', 'linux': 'ubuntu-22.04', 'macos': 'macos-26'}


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', 'repos/' + REPO + '/' + path], timeout=60))


def fetch(run_id, platform, output):
    if not re.fullmatch('[1-9][0-9]{0,19}', run_id): raise ValueError('Numeric Actions run ID required')
    if output.exists(): raise ValueError('Preserve prior evidence')
    run = api('actions/runs/' + run_id)
    if run.get('conclusion') != 'success' or run.get('status') != 'completed' or run['repository']['full_name'] != REPO:
        raise ValueError('Successful same-repository source run required')
    name = 'qt-source-replacement-' + HOSTS[platform]
    artifacts = api('actions/runs/' + run_id + '/artifacts?per_page=100')['artifacts']
    matches = [a for a in artifacts if a['name'] == name and not a['expired']]
    if len(matches) != 1: raise ValueError('One unexpired source product artifact required')
    artifact = matches[0]
    if artifact['workflow_run']['head_sha'] != run['head_sha'] or not 1 <= artifact['size_in_bytes'] <= 64 * 1024**2:
        raise ValueError('Artifact identity or size differs')
    output.mkdir(parents=True); archive = output / 'artifact.zip'
    with archive.open('xb') as stream:
        subprocess.run(['gh', 'api', f'repos/{REPO}/actions/artifacts/{artifact["id"]}/zip'], stdout=stream, check=True, timeout=60)
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    if artifact.get('digest') != 'sha256:' + sha or archive.stat().st_size != artifact['size_in_bytes']:
        raise ValueError('Artifact digest differs')
    wanted = ['linkage.json', 'payload.json' if platform == 'windows' else 'product-evidence/license-evidence.json']
    with zipfile.ZipFile(archive) as source:
        for name in wanted:
            info = source.getinfo(name)
            if info.file_size > 32 * 1024**2 or source.namelist().count(name) != 1: raise ValueError('Ambiguous or oversized evidence JSON')
            target = output / PurePosixPath(name); target.parent.mkdir(exist_ok=True, parents=True); target.write_bytes(source.read(info))
    evidence = json.loads((output / wanted[1]).read_bytes()); linkage = json.loads((output / wanted[0]).read_bytes())
    if evidence['buildIdentity'] != linkage['buildIdentity'] or evidence['buildIdentity']['buildVersion']['revision'] != run['head_sha']:
        raise ValueError('Downloaded report identity differs from source run')
    receipt = {'runId': run_id, 'sourceRevision': run['head_sha'], 'artifactId': artifact['id'], 'artifactSha256': sha,
               'artifactName': artifact['name'], 'platform': platform, 'releaseReady': False}
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--run-id', required=True); p.add_argument('--platform', choices=HOSTS, required=True)
    p.add_argument('--output', type=Path, required=True); a = p.parse_args(); print(json.dumps(fetch(a.run_id, a.platform, build_output(a.output))))
