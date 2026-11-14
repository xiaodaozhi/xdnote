import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import user_install as install

class UserInstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.root=Path(self.tmp.name);self.payload=self.root/'payload';self.payload.mkdir();(self.payload/'xdnote.exe').write_bytes(b'version-one');(self.payload/'Qt6Core.dll').write_bytes(b'fixture');self.target=self.root/'installed'
    def test_plan_does_not_mutate_and_install_upgrade_uninstall_preserve_data(self):
        self.assertFalse(install.install(self.payload,self.target,'Windows')['apply']);self.assertFalse(self.target.exists());user=self.root/'settings.json';user.write_text('keep');install.install(self.payload,self.target,'Windows',True);self.assertEqual((self.target/'xdnote.exe').read_bytes(),b'version-one');(self.payload/'xdnote.exe').write_bytes(b'version-two');self.assertEqual(install.install(self.payload,self.target,'Windows',True)['operation'],'upgrade');self.assertEqual((self.target/'xdnote.exe').read_bytes(),b'version-two');install.uninstall(self.target,True);self.assertFalse(self.target.exists());self.assertEqual(user.read_text(),'keep')
    def test_unknown_file_blocks_upgrade_and_uninstall(self):
        install.install(self.payload,self.target,'Windows',True);(self.target/'user.md').write_text('user content');
        with self.assertRaises(ValueError):install.install(self.payload,self.target,'Windows',True)
        with self.assertRaises(ValueError):install.uninstall(self.target,True)
        self.assertEqual((self.target/'user.md').read_text(),'user content')
    def test_modified_file_is_preserved(self):
        install.install(self.payload,self.target,'Windows',True);(self.target/'Qt6Core.dll').write_bytes(b'user replacement');
        with self.assertRaises(ValueError):install.uninstall(self.target,True)
        self.assertEqual((self.target/'Qt6Core.dll').read_bytes(),b'user replacement')
    def test_unowned_or_overlapping_target_is_rejected(self):
        self.target.mkdir();(self.target/'important.md').write_text('important');
        with self.assertRaises(ValueError):install.install(self.payload,self.target,'Windows',True)
        with self.assertRaises(ValueError):install.install(self.payload,self.payload/'nested','Windows',True)
        self.assertEqual((self.target/'important.md').read_text(),'important')
    def test_registry_plan_never_touches_defaults_and_quotes_unicode_paths(self):
        exe='C:\\Program Files\\中文\\xdnote.exe';plan=install.registration_plan(exe);self.assertTrue(any('RegisteredApplications' in p for p,_,_ in plan));self.assertTrue(any(v=='"'+exe+'" "%1"' for _,_,v in plan));
        for path,name,_ in plan:
            self.assertNotIn('UserChoice',path);self.assertFalse(path.endswith('\\.md') or path.endswith('\\.markdown'))
        with self.assertRaises(ValueError):install.registration_plan('C:\\bad"path')
    def test_linux_registration_does_not_set_default(self):
        (self.payload/'bin').mkdir();(self.payload/'bin/xdnote').write_bytes(b'elf-fixture');data=self.root/'data';install.install(self.payload,self.target,'Linux',True,True,data);desktop=data/'applications/io.github.xiaodaozhi.xdnote.desktop';self.assertIn(' %F',desktop.read_text());self.assertFalse((data/'applications/mimeapps.list').exists());install.uninstall(self.target,True,True,data);self.assertFalse(desktop.exists())
    def test_existing_desktop_entry_is_preserved(self):
        desktop=self.root/'data/applications/io.github.xiaodaozhi.xdnote.desktop';desktop.parent.mkdir(parents=True);desktop.write_text('other');
        with self.assertRaises(ValueError):install.integrate(self.target,'Linux',data_home=self.root/'data')
        self.assertEqual(desktop.read_text(),'other')
    def test_manifest_owner_must_match(self):
        install.install(self.payload,self.target,'Windows',True);manifest=self.target/install.MANIFEST;data=json.loads(manifest.read_text());data['owner']='other';manifest.write_text(json.dumps(data));
        with self.assertRaises(ValueError):install.uninstall(self.target,True)
    def test_failed_directory_switch_restores_original_installation(self):
        install.install(self.payload,self.target,'Windows',True);(self.payload/'xdnote.exe').write_bytes(b'next');rename=Path.rename
        def fail_stage(path,target):
            if path.name.startswith('.xdnote-stage-'):raise PermissionError('in use fixture')
            return rename(path,target)
        with patch.object(Path,'rename',fail_stage):
            with self.assertRaises(PermissionError):install.install(self.payload,self.target,'Windows',True)
        self.assertEqual((self.target/'xdnote.exe').read_bytes(),b'version-one');self.assertEqual(list(self.root.glob('.xdnote-*')),[])
    def test_registration_failure_reports_installed_files_and_preserves_entry(self):
        (self.payload/'bin').mkdir();(self.payload/'bin/xdnote').write_bytes(b'fixture');data=self.root/'data';entry=data/'applications/io.github.xiaodaozhi.xdnote.desktop';entry.parent.mkdir(parents=True);entry.write_text('other app');result=install.install(self.payload,self.target,'Linux',True,True,data);self.assertFalse(result['integrationComplete']);self.assertTrue(result['integrationError']);self.assertTrue((self.target/install.MANIFEST).exists());self.assertEqual(entry.read_text(),'other app')

if __name__=='__main__':unittest.main()
