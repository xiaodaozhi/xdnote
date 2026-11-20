"""Candidate/package inputs are bound to exact files, version and identities."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from unittest.mock import patch
import subprocess
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_candidate as release
import msix_package as msix
import build_linux_installer as linux


class ReleasePackageTests(unittest.TestCase):
    def test_usrmerge_resolves_only_the_same_actual_system_file(self):
        with patch.object(Path, 'is_file', return_value=True), patch.object(Path, 'samefile', return_value=True), patch.object(linux.subprocess, 'run', side_effect=[
            subprocess.CompletedProcess([],1,'',''), subprocess.CompletedProcess([],0,'libc6:amd64: /lib/x86_64-linux-gnu/libm.so.6\n','')]) as query:
            self.assertEqual(linux.system_packages('/usr/lib/x86_64-linux-gnu/libm.so.6'), {'libc6'})
            self.assertEqual(query.call_args.args[0][-1], '/lib/x86_64-linux-gnu/libm.so.6')
        with patch.object(Path, 'is_file', return_value=True), patch.object(Path, 'samefile', return_value=False), patch.object(linux.subprocess, 'run', return_value=subprocess.CompletedProcess([],1,'','')) as query:
            with self.assertRaises(ValueError): linux.system_packages('/usr/lib/x86_64-linux-gnu/libm.so.6')
            self.assertEqual(query.call_count,1)

    def test_six_assets_and_stale_verification_fail_closed(self):
        identity = dict(product='xdnote', version='0.2.0', build=12, revision='a'*40, dirty=False, channel='stable', configuration='Release')
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); evidence={'identity':identity, 'assets':{}}
            for _,_,_,suffix,_ in release.TARGETS:
                name='xdnote-0.2.0-'+suffix; data=('owned fixture '+suffix).encode(); (root/name).write_bytes(data)
                evidence['assets'][name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'verified':True}
            value=release.manifest(identity, root, evidence)
            self.assertEqual(len(value['assets']),6)
            self.assertEqual(value['assets'][2]['minimum_system'],{'macos':'14.0'})
            (root/value['assets'][0]['filename']).write_bytes(b'changed')
            with self.assertRaises(ValueError): release.manifest(identity,root,evidence)
            for change in ({'dirty':True},{'build':0},{'revision':'unknown'},{'channel':'development'}):
                with self.assertRaises(ValueError): release.validate_identity(identity|change)

    def test_msix_uses_explicit_identity_1809_and_nondefault_markdown_activation(self):
        # This is an XML unit fixture, never a Partner Center identity.
        identity={'name':'Example.TestFixture','publisher':'CN=Fixture Only','publisherDisplayName':'Fixture'}
        root=ET.fromstring(msix.manifest(identity,12)); node=root.find(msix.tag(msix.NS,'Identity'))
        self.assertEqual(node.attrib['Version'],'1.0.12.0'); self.assertEqual(node.attrib['ProcessorArchitecture'],'x64')
        self.assertEqual(root.find('.//'+msix.tag(msix.NS,'TargetDeviceFamily')).attrib['MinVersion'],'10.0.17763.0')
        association=root.find('.//'+msix.tag(msix.UAP3,'FileTypeAssociation'))
        self.assertEqual(association.attrib['Parameters'],'-- "%1"')
        self.assertEqual(association.attrib[msix.tag(msix.D2,'AllowSilentDefaultTakeOver')],'true')
        self.assertEqual([x.text for x in association.iter(msix.tag(msix.UAP,'FileType'))],['.md','.markdown'])
        for change in ({'name':''},{'publisher':''},{'publisherDisplayName':''},{'extra':1}):
            with self.assertRaises(ValueError): msix.manifest(identity|change,12)
        for build in (0,65536,True):
            with self.assertRaises(ValueError): msix.manifest(identity,build)

    def test_unsigned_msix_rejects_extra_or_missing_executable(self):
        xml = b'<Package/>'
        with tempfile.TemporaryDirectory() as folder:
            package = Path(folder) / 'test.msix'
            for entries in (['xdnote.exe'], [], ['xdnote.exe', 'tools/xdnote-update-helper.EXE']):
                with self.subTest(entries=entries):
                    with zipfile.ZipFile(package, 'w') as archive:
                        archive.writestr('AppxManifest.xml', xml)
                        archive.writestr('platforms/qwindows.dll', b'fixture')
                        for name in entries:
                            archive.writestr(name, b'fixture')
                    if entries == ['xdnote.exe']:
                        self.assertEqual(msix.verify_unsigned_archive(package, xml), entries)
                    else:
                        with self.assertRaisesRegex(ValueError, 'MSIX must contain only xdnote.exe'):
                            msix.verify_unsigned_archive(package, xml)


if __name__=='__main__': unittest.main()
