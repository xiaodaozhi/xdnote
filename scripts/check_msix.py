"""Exercise unsigned MSIX packaging with an explicit disposable CI identity only."""
import argparse
import json
import os
from pathlib import Path
import platform
import desktop
from installer_common import ROOT, build_output
import msix_package


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir','qt-prefix','payload','audit','work-dir'): p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    if os.environ.get('CI')!='true' or platform.system()!='Windows': p.error('Restricted to disposable Windows CI; no signing or installation')
    build=desktop.build_path(a.build_dir); qt=desktop.checked_tool_path(a.qt_prefix,'Qt SDK',sdk=True); desktop.verify_build_identity(build,'default',qt)
    work=build_output(a.work_dir)
    if work.exists(): p.error('Preserve existing evidence')
    work.mkdir(parents=True); assets=work/'assets'
    msix_package.render_assets(build,qt,assets,work/'assets.log')
    identity={'name':'Xdnote.EngineeringFixture','publisher':'CN=Xdnote Engineering Fixture Only','publisherDisplayName':'Engineering fixture'}
    report=msix_package.build(build_output(a.payload),json.loads(a.audit.read_bytes()),identity,1,assets,work/'unsigned-fixture.msix')
    report['scope']='unsigned MakeAppx/schema/asset fixture; identity is not a Partner Center assignment; no installation, signing or Store submission'
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
