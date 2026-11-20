"""Prepare independent source access metadata; never copy Qt sources to payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import time
import urllib.request
from installer_common import ROOT, build_output, digest


def config():
    value=json.loads((ROOT/'tools/qt-source-access.json').read_text(encoding='utf-8'))
    version=json.loads((ROOT/'tools/desktop-dependencies.json').read_text(encoding='utf-8'))['qt']['version']
    if value['qtVersion']!=version or value['sourceCodeInRelease'] is not False: raise ValueError('Source access configuration differs from product policy')
    for record in value['modules']:
        if record['version']!=version or not record['url'].startswith('https://download.qt.io/archive/qt/') or '/' in record['archive']: raise ValueError('Unbounded Qt source locator')
    return value


def project_source(identity):
    version = (identity or {}).get('buildVersion', {})
    revision = version.get('revision')
    if version.get('dirty') is False and isinstance(revision, str) and re.fullmatch('[0-9a-f]{40}', revision):
        return {'revision': revision, 'tree': 'https://github.com/xiaodaozhi/xdnote/tree/' + revision}
    return None


def manifest(modules, network_backports=None, cocoa_backports=None, replacement_sha256=None, *, identity=None):
    value=config()
    names={record['sbom'].split('-'+value['qtVersion'])[0] for record in modules}
    selected=[r for r in value['modules'] if r['module'] in names]
    if {r['module'] for r in selected}!=names: raise ValueError('Shipped Qt module lacks independent source locator')
    source=ROOT/'docs/engineering/qt-library-replacement.md'
    result={**value,'modules':selected,'replacementInstructionsSha256':replacement_sha256 or digest(source),'sourceAccessFulfillmentReviewComplete':False}
    if network_backports is not None:
        result['networkBackports'] = network_backports
    if cocoa_backports is not None:
        result['cocoaBackports'] = cocoa_backports
    project = project_source(identity)
    if project is not None:
        result['projectSource'] = project
    return result


def collect(destination, modules, qt=None, *, identity=None):
    destination=Path(destination)
    network_backports = cocoa_backports = None
    if qt is not None:
        from qt_network_backports import public_record
        network_backports = public_record(qt)
        if platform.system() == 'Darwin':
            from qt_cocoa_backports import public_record as cocoa_record
            cocoa_backports = cocoa_record(qt)
    record = manifest(modules, network_backports, cocoa_backports, identity=identity)
    destination.mkdir(parents=True,exist_ok=True)
    (destination/'source-access.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    shutil.copyfile(ROOT/'docs/engineering/qt-library-replacement.md',destination/'REPLACEMENT.md')
    return record


def verify(destination, qt=None, *, identity=None):
    destination=Path(destination); record=json.loads((destination/'source-access.json').read_text(encoding='utf-8')); value=config()
    if record['qtVersion']!=value['qtVersion'] or record['sourceCodeInRelease'] is not False: raise ValueError('Source access metadata changed')
    if not record['modules'] or any(r not in value['modules'] for r in record['modules']): raise ValueError('Unknown source locator')
    if 'projectSource' in record:
        reference = record['projectSource']
        if not isinstance(reference, dict):
            raise ValueError('Invalid project source reference')
        expected = project_source(identity if identity is not None else {
            'buildVersion': {'revision': reference.get('revision'), 'dirty': False}})
        if reference != expected:
            raise ValueError('Project source reference differs from package identity')
    if record['replacementInstructionsSha256']!=digest(ROOT/'docs/engineering/qt-library-replacement.md') or digest(destination/'REPLACEMENT.md')!=record['replacementInstructionsSha256']: raise ValueError('Replacement instructions changed')
    if qt is not None and (qt / 'xdnote-network-backports.json').is_file():
        from qt_network_backports import public_record
        if record.get('networkBackports') != public_record(qt): raise ValueError('Qt Network source derivation differs')
    if qt is not None and platform.system() == 'Darwin':
        from qt_cocoa_backports import public_record as cocoa_record
        if record.get('cocoaBackports') != cocoa_record(qt): raise ValueError('Qt Cocoa source derivation differs')
    return record


def check_downloads(destination, output):
    manifest=verify(destination); output=build_output(output)
    if output.exists(): raise ValueError('Preserve existing independent source cache')
    output.mkdir(parents=True); records=[]
    for item in manifest['modules']:
        target=output/item['archive']; partial=target.with_name(target.name+'.part'); hasher=hashlib.sha256(); count=0
        deadline=time.monotonic()+180
        print('Checking independent source: '+item['module'],flush=True)
        with urllib.request.urlopen(item['url'],timeout=20) as response, partial.open('xb') as stream:
            for chunk in iter(lambda:response.read1(64*1024),b''):
                if time.monotonic()>deadline: raise TimeoutError('Independent source transfer exceeded 180 seconds')
                count+=len(chunk)
                if count>256*1024*1024: raise ValueError('Source archive exceeds bound')
                hasher.update(chunk); stream.write(chunk)
        if hasher.hexdigest()!=item['sha256']: raise ValueError('Official Qt source archive hash mismatch')
        partial.rename(target)
        records.append({**item,'bytes':count,'verified':True})
    result={'ok':True,'archives':records,'sourceAccessFulfillmentReviewComplete':False,'sourceCodeInRelease':False}
    (output/'receipt.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--materials',type=Path,required=True); p.add_argument('--download-cache',type=Path,required=True)
    a=p.parse_args(); print(json.dumps(check_downloads(build_output(a.materials),a.download_cache)))
