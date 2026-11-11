"""Exercise import closure without loading inspected binaries or using SDKs."""
import importlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
deployment = importlib.import_module("deployment_inventory")


def image(normal="missing.dll", delayed="delay.dll"):
    data = bytearray(2048)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 134, 1)
    struct.pack_into("<H", data, 148, 240)
    struct.pack_into("<H", data, 152, 0x20b)
    struct.pack_into("<Q", data, 176, 0x140000000)
    struct.pack_into("<I", data, 260, 16)
    struct.pack_into("<II", data, 272, 4096, 40)
    struct.pack_into("<II", data, 368, 4160, 64)
    struct.pack_into("<IIII", data, 400, 1536, 4096, 1536, 512)
    struct.pack_into("<IIIII", data, 512, 0, 0, 0, 4256, 0)
    struct.pack_into("<IIIIIIII", data, 576, 1, 4320, 0, 0, 0, 0, 0, 0)
    data[672:672 + len(normal) + 1] = normal.encode() + b"\0"
    data[736:736 + len(delayed) + 1] = delayed.encode() + b"\0"
    return data


class DeploymentTests(unittest.TestCase):
    def test_normal_and_delay_imports_and_missing_dependencies(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stage, system = root / "stage", root / "system"
            stage.mkdir(); system.mkdir()
            path = stage / "app.exe"
            path.write_bytes(image())
            self.assertEqual(deployment.pe_imports(path), ["delay.dll", "missing.dll"])
            (system / "delay.dll").touch()
            _, missing = deployment.dependency_closure(stage, system)
            self.assertEqual(missing, ["missing.dll"])

    def test_qt_matching_modules_preserve_gaps_and_unmatched_libraries_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); qt = root / "qt"; stage = root / "stage"
            (qt / "sbom").mkdir(parents=True); stage.mkdir()
            (stage / "Qt6Core.dll").touch()
            value = {"files": [{"fileName": "./bin/Qt6Core.dll"}],
                     "packages": [{"name": "unknown", "licenseDeclared": "NOASSERTION"}],
                     "hasExtractedLicensingInfos": [{"licenseId": "MIT", "extractedText": "upstream text"}]}
            (qt / "sbom/base.spdx.json").write_text(json.dumps(value), encoding="utf-8")
            records = deployment.collect_qt_notices(qt, stage)
            self.assertEqual(records[0]["matchedBinaryNames"], ["Qt6Core.dll"])
            self.assertEqual(records[0]["packagesWithUnresolvedLicenseFields"], ["unknown"])
            self.assertIn("upstream text", (stage / "licenses/qt/UPSTREAM-NOTICES.txt").read_text())
            stage = root / "second"; stage.mkdir()
            (stage / "Qt6Core.dll").touch()
            (stage / "Qt6Unknown.dll").touch()
            with self.assertRaisesRegex(RuntimeError, "lack SDK SBOM"):
                deployment.collect_qt_notices(qt, stage)

    def test_invalid_or_truncated_imports_are_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "bad.exe"
            for content in (b"MZ", image("../outside.dll"), image()[:530]):
                path.write_bytes(content)
                with self.subTest(size=len(content)), self.assertRaises((ValueError, struct.error)):
                    deployment.pe_imports(path)


if __name__ == "__main__":
    unittest.main()
