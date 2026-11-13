"""Build relocated Unix diagnostic staging, not an installer or release.

Uses the project's pinned Qt SDK plus host inspection tools. Every dynamic
dependency must resolve to staging or an explicitly recorded system location.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess

import desktop
from background_validation import check_report, execute, offscreen_environment
from check_third_party_licenses import audit
from deployment_inventory import collect_qt_notices, digest

PROGRAMS = ("xdnote", "xdnote_background_probe", "xdnote_preview_product_probe", "xdnote-resource-helper", "xdnote_desktop_tests")
FORBIDDEN = ("webengine", "chromium", "chrome_elf", "libcef", "electron", "webview2")
MACH_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def inspect(command):
    return subprocess.check_output([str(arg) for arg in command], text=True, encoding="utf-8", timeout=30)


def staged_files(root):
    """Framework links are allowed only when relative and contained in staging."""
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.resolve(strict=True).is_relative_to(root):
                raise ValueError("Staging link escapes its directory: " + str(path))
            yield path
        elif path.is_file():
            yield path


def binary(path, system):
    if path.is_symlink():
        return False
    with path.open("rb") as file:
        magic = file.read(4)
    return magic == b"\x7fELF" if system == "Linux" else magic in MACH_MAGICS


def elf_dynamic(text):
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", text)
    if any("/" in name or name in ("", ".", "..") for name in needed):
        raise ValueError("ELF dependency is not a library name")
    paths = [item for row in re.findall(r"\((?:RPATH|RUNPATH)\).*?\[([^\]]*)\]", text) for item in row.split(":")]
    return needed, paths


def macho_loads(text):
    """Read load commands rather than guessing identities from otool -L rows."""
    imports, paths, identity = [], [], None
    for block in re.split(r"\nLoad command \d+\n", text):
        command = re.search(r"^\s+cmd (LC_\w+)$", block, re.MULTILINE)
        if not command:
            continue
        name = re.search(r"^\s+name (.+) \(offset \d+\)$", block, re.MULTILINE)
        if command[1] in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
            if not name:
                raise ValueError("Malformed Mach-O dylib command")
            imports.append(name[1])
        elif command[1] == "LC_ID_DYLIB" and name:
            identity = name[1]
        elif command[1] == "LC_RPATH":
            match = re.search(r"^\s+path (.+) \(offset \d+\)$", block, re.MULTILINE)
            if not match:
                raise ValueError("Malformed Mach-O rpath command")
            paths.append(match[1])
    return list(dict.fromkeys(imports)), list(dict.fromkeys(paths)), identity


def clean_environment():
    blocked = ("QT", "QML", "LD_", "DYLD_", "PYTHON")
    result = {name: value for name, value in os.environ.items() if not name.upper().startswith(blocked)}
    result["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    return result


def copy_programs(build, target):
    target.mkdir(parents=True)
    for name in PROGRAMS:
        source = build / name
        if not source.is_file():
            source = build / (name + ".app") / "Contents/MacOS" / name
        if not source.is_file() or source.is_symlink():
            raise ValueError("Missing ordinary build program: " + name)
        shutil.copy2(source, target / name)


def linux_stage(build, qt, destination, evidence):
    bindir, libdir, plugins = destination / "bin", destination / "lib", destination / "plugins"
    copy_programs(build, bindir)
    # Keep the native platform candidates and the diagnostic plugin. Other
    # device backends are outside this prototype's delivery scope.
    wanted = ("platforms/libqoffscreen.so", "platforms/libqxcb.so", "platforms/libqwayland-generic.so", "platforms/libqwayland-egl.so",
              "imageformats/libqjpeg.so", "imageformats/libqsvg.so", "iconengines/libqsvgicon.so")
    for relative in wanted:
        source = qt / "plugins" / relative
        if not source.is_file():
            if relative.startswith("platforms/"):
                raise ValueError("Missing pinned platform plugin: " + relative)
            continue
        target = plugins / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for category in ("wayland-decoration-client", "wayland-graphics-integration-client", "xcbglintegrations"):
        source = qt / "plugins" / category
        if source.is_dir():
            shutil.copytree(source, plugins / category, symlinks=False)
    libdir.mkdir()
    readelf = desktop.prepared_tool("readelf", os.environ)
    patchelf = desktop.prepared_tool("patchelf", os.environ)
    pending = [path for path in staged_files(destination) if binary(path, "Linux")]
    done = set()
    while pending:
        path = pending.pop()
        if path in done:
            continue
        done.add(path)
        names, _ = elf_dynamic(inspect([readelf, "-d", path]))
        for name in names:
            source = qt / "lib" / name
            if source.is_file() and not (libdir / name).exists():
                actual = source.resolve(strict=True)
                if not actual.is_relative_to(qt):
                    raise ValueError("Qt library link escapes its SDK")
                if not (libdir / actual.name).exists():
                    shutil.copy2(actual, libdir / actual.name)
                    pending.append(libdir / actual.name)
                if actual.name != name:
                    (libdir / name).symlink_to(actual.name)
        relative = os.path.relpath(libdir, path.parent)
        rpath = "$ORIGIN" if relative == "." else "$ORIGIN/" + relative
        inspect([patchelf, "--set-rpath", rpath, path])
    (bindir / "qt.conf").write_text("[Paths]\nPrefix=..\nPlugins=plugins\nQmlImports=qml\n", encoding="utf-8")
    return bindir, plugins, libdir


def mac_stage(build, qt, destination, evidence):
    bundle = destination / "xdnote.app"
    contents = bundle / "Contents"
    bindir = contents / "MacOS"
    copy_programs(build, bindir)
    resources = contents / "Resources"
    resources.mkdir()
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump({"CFBundleExecutable": "xdnote", "CFBundleIdentifier": "io.github.xiaodaozhi.xdnote.diagnostic",
                      "CFBundleName": "xdnote", "CFBundlePackageType": "APPL", "CFBundleVersion": "0",
                      "LSMinimumSystemVersion": "13.0", "NSHighResolutionCapable": True,
                      "CFBundleDocumentTypes": [{"CFBundleTypeName": "Markdown", "CFBundleTypeRole": "Editor", "LSHandlerRank": "Alternate", "LSItemContentTypes": ["net.daringfireball.markdown"]}],
                      "UTImportedTypeDeclarations": [{"UTTypeIdentifier": "net.daringfireball.markdown", "UTTypeDescription": "Markdown", "UTTypeConformsTo": ["public.plain-text"], "UTTypeTagSpecification": {"public.filename-extension": ["md", "markdown"], "public.mime-type": "text/markdown"}}]}, file)
    env = desktop.runtime_environment(desktop.environment(None), qt)
    # Ad-hoc integrity seals permit relocated arm64 code to execute. No identity,
    # entitlements, timestamp, notarization, disk image or release is requested.
    command = [qt / "bin/macdeployqt", bundle, "-verbose=2", "-no-strip", "-codesign=-"]
    command += ["-executable=" + str(bindir / name) for name in PROGRAMS[1:]]
    command += ["-qmldir=" + str(desktop.ROOT / "apps/desktop/ui")]
    execute(command, env, evidence / "macdeployqt.log", 240)
    plugins, frameworks = contents / "PlugIns", contents / "Frameworks"
    # macdeployqt scans all SDK SQL drivers, including commercial database
    # clients absent from the SDK. The product has no Qt SQL/database feature.
    sql = plugins / "sqldrivers"
    if sql.exists():
        if sql.is_symlink() or not sql.resolve().is_relative_to(destination):
            raise ValueError("Unexpected SQL plugin directory")
        shutil.rmtree(sql)
    # macdeployqt may not infer the offscreen backend from GUI dependencies.
    offscreen = plugins / "platforms/libqoffscreen.dylib"
    if not offscreen.is_file():
        offscreen.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(qt / "plugins/platforms/libqoffscreen.dylib", offscreen)
    (resources / "qt.conf").write_text("[Paths]\nPlugins=PlugIns\nQmlImports=Resources/qml\n", encoding="utf-8")
    install_name = desktop.prepared_tool("install_name_tool", os.environ)
    codesign = desktop.prepared_tool("codesign", os.environ)
    # Remove build/SDK rpaths and force every copied Mach-O to the local framework
    # directory. Patch SDK absolute imports only if the matching framework exists.
    binaries = [path for path in staged_files(destination) if binary(path, "Darwin")]
    for path in binaries:
        imports, paths, _ = macho_loads(inspect(["/usr/bin/otool", "-l", path]))
        for entry in paths:
            inspect([install_name, "-delete_rpath", entry, path])
        local = "@loader_path/" + os.path.relpath(frameworks, path.parent)
        inspect([install_name, "-add_rpath", local, path])
        for name in imports:
            if name.startswith(str(qt / "lib") + "/"):
                relative = name[len(str(qt / "lib")) + 1:]
                if not (frameworks / relative).is_file():
                    raise RuntimeError("Missing framework for relocated import: " + name)
                inspect([install_name, "-change", name, "@rpath/" + relative, path])
        inspect([codesign, "--force", "--sign", "-", path])
    inspect([codesign, "--force", "--deep", "--sign", "-", bundle])
    return bindir, plugins, frameworks


def resolve_macho(name, path, bindir, rpaths, root):
    def expand(value):
        return Path(value.replace("@loader_path", str(path.parent)).replace("@executable_path", str(bindir)))
    candidates = [expand(value) / name[len("@rpath/"):] for value in rpaths] if name.startswith("@rpath/") else [expand(name)]
    for candidate in candidates:
        if candidate.is_file() and candidate.resolve().is_relative_to(root):
            return "deployed", candidate.resolve().relative_to(root).as_posix()
    if name.startswith(("/usr/lib/", "/System/Library/")):
        # dyld shared-cache libraries need not exist as individual disk files.
        return "host macOS system library", name
    raise RuntimeError("Unresolved or external Mach-O dependency: " + name)


def unix_closure(root, bindir, system):
    imports, names = [], set()
    cache = {}
    if system == "Linux":
        for line in inspect(["/sbin/ldconfig", "-p"]).splitlines():
            match = re.match(r"\s*(\S+)\s+\([^)]*\)\s+=>\s+(\S+)", line)
            if match:
                cache.setdefault(match[1], Path(match[2]))
    for path in staged_files(root):
        if path.is_symlink() or not binary(path, system):
            continue
        names.add(path.name.lower())
        if system == "Linux":
            needed, rpaths = elf_dynamic(inspect(["/usr/bin/readelf", "-d", path]))
            if not rpaths or any(not entry.startswith("$ORIGIN") for entry in rpaths):
                raise RuntimeError("ELF search path escapes staging: " + str(path))
            for name in needed:
                candidates = [Path(entry.replace("$ORIGIN", str(path.parent))) / name for entry in rpaths]
                found = next((item for item in candidates if item.is_file() and item.resolve().is_relative_to(root)), None)
                if found:
                    kind, resolved = "deployed", found.resolve().relative_to(root).as_posix()
                elif not name.startswith("libQt") and name in cache and any(cache[name].resolve().is_relative_to(base) for base in ("/usr/lib", "/lib")):
                    kind, resolved = "host Linux system library", str(cache[name].resolve())
                else:
                    raise RuntimeError("Unresolved or external ELF dependency: " + name)
                imports.append({"file": path.relative_to(root).as_posix(), "import": name, "resolution": kind, "resolved": resolved})
        else:
            needed, rpaths, _ = macho_loads(inspect(["/usr/bin/otool", "-l", path]))
            for entry in rpaths:
                expanded = Path(entry.replace("@loader_path", str(path.parent)).replace("@executable_path", str(bindir)))
                if not expanded.resolve().is_relative_to(root):
                    raise RuntimeError("Mach-O search path escapes staging: " + entry)
            for name in needed:
                try:
                    kind, resolved = resolve_macho(name, path, bindir, rpaths, root)
                except RuntimeError as error:
                    raise RuntimeError(path.relative_to(root).as_posix() + ": " + str(error)) from error
                imports.append({"file": path.relative_to(root).as_posix(), "import": name, "resolution": kind, "resolved": resolved})
    return imports, names


def smoke(bindir, plugins, libraries, evidence, system):
    env = offscreen_environment(clean_environment(), evidence)
    env.update(QT_PLUGIN_PATH=str(plugins), QT_QPA_PLATFORM_PLUGIN_PATH=str(plugins / "platforms"))
    env["QML_IMPORT_PATH"] = env["QML2_IMPORT_PATH"] = str(plugins.parent / "qml")
    if system == "Linux":
        env["LD_LIBRARY_PATH"] = str(libraries)
    records = []
    commands = [("light", [bindir / PROGRAMS[1], "preview", 0, "light", evidence / "light.png"]),
                ("dark", [bindir / PROGRAMS[1], "preview", 0, "dark", evidence / "dark.png"]),
                ("product", [bindir / PROGRAMS[2], 10240, "mixed", evidence / "product.png"]),
                ("image", [bindir / PROGRAMS[1], "image", 0, "generated"])]
    for label, command in commands:
        output, _ = execute(command, env, evidence / ("smoke-" + label + ".log"), 90)
        result = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
        if len(result) != 1 or result[0].get("ok") is not True:
            raise RuntimeError("Staged diagnostic did not return success: " + label)
        records.append({"case": label, "result": result[0]})
    report = evidence / "resource-qtest.txt"
    execute([bindir / "xdnote_desktop_tests", "workspaceImagesUseAuthorizedSnapshotsAndStaticDecoding",
             "-o", "-,txt", "-o", str(report) + ",txt"], env, evidence / "smoke-resource.log", 90)
    records.append({"case": "resource-helper", "passedIncludingFixtures": check_report(report.read_text(encoding="utf-8"))})
    return records


def prepare(build, qt, destination):
    system = platform.system()
    identity = desktop.verify_build_identity(build, "default", qt)
    if audit(desktop.ROOT):
        raise RuntimeError("Source license inputs failed verification")
    evidence = destination.parent / (destination.name + "-evidence")
    if any(path.exists() or path.is_symlink() for path in (destination, evidence)):
        raise ValueError("Choose new staging and evidence paths; existing content is never overwritten")
    destination.mkdir(parents=True)
    evidence.mkdir()
    result = {"schemaVersion": 1, "kind": "Unix diagnostic staging; not an installer or release", "ok": False,
              "host": {"system": system, "release": platform.release(), "architecture": platform.machine()},
              "buildIdentity": identity, "toolInputs": {name: digest(desktop.ROOT / name) for name in
                  ("scripts/deployment_unix.py", "scripts/deployment_inventory.py", "tools/deployment-notices.json")},
              "releaseReady": False, "licenseReviewComplete": False,
              "remaining": ["final license/source/replacement obligations", "minimum OS", "native platform and desktop acceptance",
                            "installer/upgrade/uninstall and file association", "identity signing/notarization and release policy"],
              "references": ["https://doc.qt.io/qt-6.8/linux-deployment.html", "https://doc.qt.io/qt-6.8/macos-deployment.html"]}
    try:
        bindir, plugins, libraries = (linux_stage if system == "Linux" else mac_stage)(build, qt, destination, evidence)
        imports, names = unix_closure(destination, bindir, system)
        licenses = destination / "licenses"
        shutil.copytree(desktop.ROOT / "LICENSES", licenses / "project-and-source-dependencies")
        shutil.copy2(desktop.ROOT / "LICENSE", licenses / "Apache-2.0.txt")
        shutil.copy2(desktop.ROOT / "THIRD_PARTY_NOTICES.md", licenses / "THIRD_PARTY_NOTICES.md")
        (licenses / "DESIGN-ATTRIBUTION.txt").write_text("xdnote 项目及其贡献者；https://github.com/xiaodaozhi/xdnote\n文档与原创设计：CC BY 4.0；https://creativecommons.org/licenses/by/4.0/\n", encoding="utf-8")
        result["qtSbom"] = collect_qt_notices(qt, destination, names)
        forbidden = [path.relative_to(destination).as_posix() for path in staged_files(destination)
                     if any(token in path.relative_to(destination).as_posix().lower() for token in FORBIDDEN)]
        if forbidden:
            raise RuntimeError("Forbidden browser runtime in staging: " + ", ".join(forbidden))
        result.update(imports=imports, missingImports=[], browserRuntimeFiles=forbidden,
                      offscreenSmoke=smoke(bindir, plugins, libraries, evidence, system),
                      signing="ad-hoc local integrity only; no identity or notarization" if system == "Darwin" else "none")
        result["excludedOptionalPlugins"] = {"sqldrivers": "No Qt SQL database feature; do not admit external database clients"} if system == "Darwin" else {}
        result["files"] = [{"path": path.relative_to(destination).as_posix(), "link": os.readlink(path)} if path.is_symlink()
                           else {"path": path.relative_to(destination).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
                           for path in staged_files(destination)]
        result["ok"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (evidence / "inventory.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(evidence / "inventory.json"), "ok": result["ok"], "files": len(result["files"]),
                      "imports": len(result["imports"]), "releaseReady": False}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--qt-prefix", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    args = parser.parse_args()
    if platform.system() not in ("Linux", "Darwin"):
        parser.error("Unix staging requires the corresponding Linux or macOS host")
    prepare(desktop.build_path(args.build_dir), desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True),
            desktop.build_path(args.destination))
