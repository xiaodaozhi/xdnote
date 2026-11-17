"""Runtime pruning prevents SDK headers from entering developer archives."""
import json
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import sparkle_bundle
import developer_archive
from product_deployment_unix import inventory


class SparkleRuntimeTests(unittest.TestCase):
    def test_archive_contains_runtime_and_not_framework_sdk_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); sdk=root/'sdk'; framework=sdk/'Sparkle.framework'; version=framework/'Versions/B'
            for name in ('Headers','PrivateHeaders','Modules','XPCServices','Updater.app/Contents/MacOS'): (version/name).mkdir(parents=True)
            for name in ('Headers','PrivateHeaders','Modules','XPCServices'): (framework/name).symlink_to('Versions/B/'+name)
            for name in ('Headers/API.h','PrivateHeaders/Private.h','Modules/module.modulemap','Autoupdate','Sparkle','Updater.app/Contents/MacOS/Updater'): (version/name).write_bytes(b'owned source fixture')
            (sdk/'LICENSE').write_text('fixture'); (sdk/'xdnote-sdk.json').write_text('{}')
            product=root/'product'; app=product/'xdnote.app'; (app/'Contents/Frameworks').mkdir(parents=True); (app/'Contents/Resources').mkdir()
            (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'fixture'})); evidence=root/'evidence'; evidence.mkdir()
            with patch.object(sparkle_bundle,'sdk',return_value=sdk):
                result=sparkle_bundle.embed(app,lambda args:'',evidence,{'version':'0.1.0-dev.fixture','channel':'development','build':0,'dirty':False})
            self.assertTrue((result/'Versions/B/Autoupdate').is_file())
            self.assertTrue((version/'Headers/API.h').is_file())
            self.assertFalse((result/'Versions/B/Headers').exists()); self.assertFalse((result/'Headers').is_symlink())
            developer_archive.verify(product,{'ok':True,'files':inventory(product)})


if __name__=='__main__': unittest.main()
