"""Unix relocation contracts without running native binaries on other hosts."""
import importlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
deploy = importlib.import_module("deployment_unix")


class UnixDeploymentTests(unittest.TestCase):
    def test_elf_needed_and_both_search_path_commands(self):
        text = "0x1 (NEEDED) Shared library: [libQt6Core.so.6]\n0x1d (RUNPATH) Library runpath: [$ORIGIN/../lib:$ORIGIN]\n"
        self.assertEqual(deploy.elf_dynamic(text), (["libQt6Core.so.6"], ["$ORIGIN/../lib", "$ORIGIN"]))
        self.assertEqual(deploy.elf_dynamic(text.replace("RUNPATH", "RPATH")), deploy.elf_dynamic(text))
        with self.assertRaises(ValueError):
            deploy.elf_dynamic("(NEEDED) Shared library: [/build/Qt/libQt6Core.so.6]")

    def test_macho_dependencies_do_not_confuse_install_identity(self):
        text = """header:
Load command 0
          cmd LC_ID_DYLIB
         name @rpath/QtCore.framework/Versions/A/QtCore (offset 24)
Load command 1
          cmd LC_LOAD_DYLIB
         name /usr/lib/libSystem.B.dylib (offset 24)
Load command 2
          cmd LC_RPATH
         path @loader_path/../Frameworks (offset 12)
Load command 3
          cmd LC_LOAD_WEAK_DYLIB
         name @rpath/QtGui.framework/Versions/A/QtGui (offset 24)
"""
        imports, paths, identity = deploy.macho_loads(text)
        self.assertEqual(imports, ["/usr/lib/libSystem.B.dylib", "@rpath/QtGui.framework/Versions/A/QtGui"])
        self.assertEqual(paths, ["@loader_path/../Frameworks"])
        self.assertEqual(identity, "@rpath/QtCore.framework/Versions/A/QtCore")
        with self.assertRaises(ValueError):
            deploy.macho_loads(text.replace("name /usr/lib/libSystem.B.dylib", "broken"))

    def test_macho_resolution_rejects_missing_sdk_and_external_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binary = root / "Contents/MacOS/xdnote"
            frameworks = root / "Contents/Frameworks"
            binary.parent.mkdir(parents=True); frameworks.mkdir()
            (frameworks / "local.dylib").touch()
            args = (binary, binary.parent, ["@loader_path/../Frameworks"], root)
            self.assertEqual(deploy.resolve_macho("@rpath/local.dylib", *args)[0], "deployed")
            self.assertEqual(deploy.resolve_macho("/usr/lib/shared-cache-only.dylib", *args)[0], "host macOS system library")
            for name in ("@rpath/missing.dylib", "/build/Qt/QtCore", "/opt/homebrew/lib/other.dylib"):
                with self.subTest(name=name), self.assertRaises(RuntimeError):
                    deploy.resolve_macho(name, *args)

    def test_isolated_runtime_discards_sdk_preloads_and_user_platform(self):
        source = {"PATH": "/other/project/bin", "QT_PLUGIN_PATH": "/sdk/plugins", "QT_QPA_PLATFORM": "native",
                  "QML_IMPORT_PATH": "/sdk/qml", "DYLD_LIBRARY_PATH": "/sdk/lib", "LD_PRELOAD": "other.so",
                  "LD_LIBRARY_PATH": "/sdk/lib", "PYTHONPATH": "/other/project", "HOME": "preserved"}
        with patch.dict(os.environ, source, clear=True):
            env = deploy.clean_environment()
        self.assertEqual(env, {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": "preserved"})

    def test_unix_qt_sbom_matches_versioned_library_and_framework(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); qt = root / "qt"; stage = root / "stage"
            (qt / "sbom").mkdir(parents=True); stage.mkdir()
            (qt / "sbom/base.spdx.json").write_text(json.dumps({"files": [
                {"fileName": "./lib/libQt6Core.so.6.8.2"}, {"fileName": "./lib/QtCore.framework/Versions/A/QtCore"}]}))
            records = deploy.collect_qt_notices(qt, stage, {"libqt6core.so.6.8.2", "qtcore"})
            self.assertEqual(len(records), 1)
            self.assertEqual(len(records[0]["matchedBinaryNames"]), 2)
            second = root / "second"; second.mkdir()
            with self.assertRaisesRegex(RuntimeError, "lack SDK SBOM"):
                deploy.collect_qt_notices(qt, second, {"qtunknown"})

    def test_framework_links_must_be_relative_live_and_internal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); stage = root / "stage"; stage.mkdir()
            (stage / "actual").touch()
            try:
                (stage / "relative").symlink_to("actual")
            except OSError:
                self.skipTest("Host does not permit unprivileged symbolic links")
            self.assertEqual(len(list(deploy.staged_files(stage))), 2)
            (root / "outside").touch(); (stage / "bad").symlink_to("../outside")
            with self.assertRaises((ValueError, OSError)):
                list(deploy.staged_files(stage))

    def test_existing_paths_are_preserved_and_failures_keep_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); destination = root / "stage"
            destination.mkdir(); (destination / "keep").write_text("user input")
            with patch.object(deploy.desktop, "verify_build_identity", return_value={"sourceSha256": "source"}), \
                 patch.object(deploy, "audit", return_value=[]), patch.object(deploy.platform, "system", return_value="Linux"):
                with self.assertRaisesRegex(ValueError, "never overwritten"):
                    deploy.prepare(root, root, destination)
                self.assertEqual((destination / "keep").read_text(), "user input")
                with patch.object(deploy, "linux_stage", side_effect=RuntimeError("missing native plugin")), \
                     self.assertRaisesRegex(RuntimeError, "missing native plugin"):
                    deploy.prepare(root, root, root / "fresh")
            evidence = json.loads((root / "fresh-evidence/inventory.json").read_text(encoding="utf-8"))
            self.assertFalse(evidence["ok"])
            self.assertFalse(evidence["releaseReady"])
            self.assertEqual(evidence["error"], "missing native plugin")

    def test_elf_closure_rejects_sdk_search_paths_even_with_host_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); program = root / "app"
            program.write_bytes(b"\x7fELF")
            def output(command):
                return "libc.so.6 (libc6,x86-64) => /lib/x86_64-linux-gnu/libc.so.6" if "ldconfig" in str(command[0]) else "(RUNPATH) Library runpath: [/sdk/Qt/lib]"
            with patch.object(deploy, "inspect", side_effect=output), self.assertRaisesRegex(RuntimeError, "escapes staging"):
                deploy.unix_closure(root, root, "Linux")


if __name__ == "__main__":
    unittest.main()
