"""Candidate/package inputs are bound to exact files, version and identities."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_candidate as release
import msix_package as msix


class ReleasePackageTests(unittest.TestCase):
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


if __name__=='__main__': unittest.main()
