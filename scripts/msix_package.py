"""Build an unsigned Store-layout MSIX only with explicitly supplied package identity."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile

from background_validation import execute
import desktop
from installer_common import ROOT, build_output, digest
from installer_payload import verify_payload

NS = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'
UAP = 'http://schemas.microsoft.com/appx/manifest/uap/windows10'
UAP3 = UAP + '/3'
D2 = 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/2'
RES = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities'
for prefix, uri in (('', NS), ('uap', UAP), ('uap3', UAP3), ('desktop2', D2), ('rescap', RES)): ET.register_namespace(prefix, uri)

def tag(ns, key): return '{' + ns + '}' + key


def manifest(identity, build):
    if set(identity) != {'name', 'publisher', 'publisherDisplayName'}: raise ValueError('Supply the actual Partner Center identity fields')
    name, publisher, display = identity['name'], identity['publisher'], identity['publisherDisplayName']
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]{2,49}', name) or name.endswith('.'): raise ValueError('Invalid package name')
    if not publisher.startswith('CN=') or len(publisher) > 512 or any(ord(c) < 32 for c in publisher): raise ValueError('Invalid explicit Publisher')
    if not display.strip() or len(display) > 256 or any(ord(c) < 32 for c in display): raise ValueError('Invalid PublisherDisplayName')
    if type(build) is not int or not 1 <= build <= 65535: raise ValueError('MSIX build must be 1..65535')
    root = ET.Element(tag(NS, 'Package'), IgnorableNamespaces='uap uap3 desktop2 rescap')
    ET.SubElement(root, tag(NS, 'Identity'), Name=name, Publisher=publisher, Version=f'1.0.{build}.0', ProcessorArchitecture='x64')
    props = ET.SubElement(root, tag(NS, 'Properties'))
    for key, value in (('DisplayName', 'xdnote'), ('PublisherDisplayName', display), ('Logo', r'Assets\logo50.png')): ET.SubElement(props, tag(NS, key)).text = value
    deps = ET.SubElement(root, tag(NS, 'Dependencies'))
    ET.SubElement(deps, tag(NS, 'TargetDeviceFamily'), Name='Windows.Desktop', MinVersion='10.0.17763.0', MaxVersionTested='10.0.17763.0')
    resources = ET.SubElement(root, tag(NS, 'Resources'))
    for language in ('zh-CN', 'en-US'): ET.SubElement(resources, tag(NS, 'Resource'), Language=language)
    app = ET.SubElement(ET.SubElement(root, tag(NS, 'Applications')), tag(NS, 'Application'), Id='App', Executable='xdnote.exe', EntryPoint='Windows.FullTrustApplication')
    ET.SubElement(app, tag(UAP, 'VisualElements'), DisplayName='xdnote', Description='xdnote Markdown reader and editor', BackgroundColor='transparent', Square150x150Logo=r'Assets\logo150.png', Square44x44Logo=r'Assets\logo44.png')
    extension = ET.SubElement(ET.SubElement(app, tag(NS, 'Extensions')), tag(UAP3, 'Extension'), Category='windows.fileTypeAssociation')
    association = ET.SubElement(extension, tag(UAP3, 'FileTypeAssociation'), {'Name': 'xdnote-markdown', 'Parameters': '-- "%1"', tag(D2, 'AllowSilentDefaultTakeOver'): 'true'})
    types = ET.SubElement(association, tag(UAP, 'SupportedFileTypes'))
    for suffix in ('.md', '.markdown'): ET.SubElement(types, tag(UAP, 'FileType')).text = suffix
    ET.SubElement(ET.SubElement(root, tag(NS, 'Capabilities')), tag(RES, 'Capability'), Name='runFullTrust')
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def sdk_tool():
    base = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10/bin'
    tools = sorted(base.glob('10.*/x64/makeappx.exe'), key=lambda p: tuple(map(int, p.parent.parent.name.split('.'))), reverse=True)
    if not tools: raise ValueError('Windows SDK MakeAppx.exe is required')
    return tools[0]


def render_assets(build, qt, output, log):
    environment = desktop.runtime_environment(desktop.environment(None), qt)
    execute([build / 'xdnote_msix_assets.exe', ROOT / 'design/assets/ui-v2/brand/app-icon.svg', output],
            environment, log, 30)


def verify_unsigned_archive(output, xml):
    with zipfile.ZipFile(output) as archive:
        names = archive.namelist()
        if archive.read('AppxManifest.xml') != xml or 'AppxSignature.p7x' in names:
            raise ValueError('Unsigned package identity differs')
        executables = sorted(name for name in names if name.casefold().endswith('.exe'))
        if executables != ['xdnote.exe']:
            raise ValueError(f'MSIX must contain only xdnote.exe as an executable: {executables}')
        return executables


def build(payload, audit, identity, build_number, assets, output):
    verify_payload(payload, audit); xml = manifest(identity, build_number)
    if output.exists(): raise ValueError('Preserve an existing package')
    stage = output.with_suffix('.stage')
    if stage.exists(): raise ValueError('Preserve an existing package stage')
    shutil.copytree(payload, stage)
    # Store manages updates; no independent updater executable belongs in MSIX.
    (stage / 'xdnote-update-helper.exe').unlink(missing_ok=True)
    target = stage / 'Assets'; target.mkdir()
    for size in (44, 50, 150):
        source = assets / f'logo{size}.png'
        with source.open('rb') as file:
            header = file.read(24)
        if header[:8] != b'\x89PNG\r\n\x1a\n' or tuple(int.from_bytes(header[i:i+4], 'big') for i in (16,20)) != (size,size): raise ValueError('Incorrect MSIX asset dimensions')
        shutil.copy2(source, target / source.name)
    (stage / 'AppxManifest.xml').write_bytes(xml)
    subprocess.run([str(sdk_tool()), 'pack', '/d', str(stage), '/p', str(output), '/o'], check=True)
    executables = verify_unsigned_archive(output, xml)
    return {'ok': True, 'signed': False, 'storeIdentityVerified': False, 'storeSubmitted': False, 'file': output.name, 'sha256': digest(output),
            'identity': identity, 'build': build_number, 'sourceIdentity': audit.get('buildIdentity'), 'releaseReady': False,
            'executables': executables}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('root','audit','identity','assets','output','report'): p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--build', type=int, required=True); a = p.parse_args()
    report = build(build_output(a.root), json.loads(a.audit.read_bytes()), json.loads(a.identity.read_bytes()), a.build, build_output(a.assets), build_output(a.output))
    build_output(a.report).write_text(json.dumps(report, indent=2) + '\n')
