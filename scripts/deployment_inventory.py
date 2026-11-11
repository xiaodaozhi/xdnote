"""Prepare a Windows diagnostic deployment and audit its actual file closure.

No installer, system registration, signing or release. Never overwrite an
existing directory. Qt SBOM is upstream evidence, not a complete license review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct

import desktop
from check_third_party_licenses import audit
from background_validation import execute, offscreen_environment


def pe_imports(path):
    """Read normal and delay-load imports without loading the inspected DLL."""
    data = Path(path).read_bytes()
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("Truncated PE image")
        return struct.unpack_from(fmt, data, offset)
    if data[:2] != b"MZ":
        raise ValueError("Not a PE image")
    header, = unpack("<I", 0x3c)
    if data[header:header + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")
    count, = unpack("<H", header + 6)
    optional_size, = unpack("<H", header + 20)
    optional = header + 24
    magic, = unpack("<H", optional)
    if magic not in (0x10b, 0x20b):
        raise ValueError("Unsupported PE optional header")
    base, = unpack("<Q" if magic == 0x20b else "<I", optional + (24 if magic == 0x20b else 28))
    directories = optional + (112 if magic == 0x20b else 96)
    directory_count, = unpack("<I", directories - 4)
    if directories + min(directory_count, 16) * 8 > optional + optional_size:
        raise ValueError("Invalid PE directory table")
    sections = [unpack("<IIII", optional + optional_size + i * 40 + 8) for i in range(count)]
    def offset(rva):
        for virtual_size, start, raw_size, raw in sections:
            if start <= rva < start + max(virtual_size, raw_size):
                if rva - start >= raw_size:
                    raise ValueError("Import points outside raw section")
                return raw + rva - start
        raise ValueError("Import RVA is not in a section")
    def name(rva):
        start = offset(rva)
        end = data.find(b"\0", start, min(start + 512, len(data)))
        if end < 0:
            raise ValueError("Unterminated DLL name")
        value = data[start:end].decode("ascii").lower()
        if not value or any(char in value for char in "/\\:"):
            raise ValueError("Invalid DLL import name")
        return value
    imports = set()
    for index, width, fmt in ((1, 20, "<IIIII"), (13, 32, "<IIIIIIII")):
        if directory_count <= index:
            continue
        rva, size = unpack("<II", directories + index * 8)
        if not rva or not size:
            continue
        start = offset(rva)
        for i in range(min(size // width, 4096)):
            row = unpack(fmt, start + i * width)
            if not any(row):
                break
            target = row[3] if index == 1 else row[1] - (0 if row[0] & 1 else base)
            imports.add(name(target))
        else:
            raise ValueError("Unterminated or excessive import descriptors")
    return sorted(imports)


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def files_under(directory):
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("Deployment inputs cannot contain links")
        if path.is_file():
            yield path


def dependency_closure(directory, system_directory):
    files = list(files_under(directory))
    binaries = [path for path in files if path.suffix.lower() in (".dll", ".exe")]
    available = {path.name.lower() for path in binaries if path.parent == directory}
    result, missing = [], set()
    for path in binaries:
        for name in pe_imports(path):
            if name in available or (path.parent / name).is_file():
                kind = "deployed"
            elif name.startswith(("api-ms-win-", "ext-ms-win-")):
                kind = "Windows API set"
            elif (system_directory / name).is_file():
                kind = "host Windows system library"
            else:
                kind = "missing"
                missing.add(name)
            result.append({"file": path.relative_to(directory).as_posix(), "import": name, "resolution": kind})
    return result, sorted(missing)


def collect_qt_notices(qt, directory, binary_names=None):
    target = directory / "licenses/qt"
    target.mkdir(parents=True)
    shipped = set(binary_names) if binary_names is not None else {
        path.name.lower() for path in files_under(directory) if path.suffix.lower() in (".dll", ".exe")}
    records, notices = [], []
    covered = set()
    for sbom in sorted((qt / "sbom").glob("*.spdx.json")):
        value = json.loads(sbom.read_text(encoding="utf-8"))
        matched = [item for item in value.get("files", []) if Path(item.get("fileName", "")).name.lower() in shipped]
        if not matched:
            continue
        covered.update(Path(item["fileName"]).name.lower() for item in matched)
        shutil.copy2(sbom, target / sbom.name)
        records.append({"sbom": sbom.name, "sha256": digest(sbom), "matchedBinaryNames": sorted({Path(item["fileName"]).name for item in matched}),
                        "upstreamPackages": len(value.get("packages", [])),
                        "packagesWithUnresolvedLicenseFields": [item.get("name", "") for item in value.get("packages", [])
                            if any(item.get(field, "NOASSERTION") == "NOASSERTION" for field in ("licenseDeclared", "licenseConcluded"))],
                        "scope": "whole Qt module upstream SBOM; may include build tools and unused components"})
        notices.append("\n=== " + sbom.name + " ===\n")
        for package in value.get("packages", []):
            notices.append("\n" + package.get("name", "") + " " + package.get("versionInfo", "") + "\n" +
                           package.get("copyrightText", "NOASSERTION") + "\nDeclared: " + package.get("licenseDeclared", "NOASSERTION") +
                           "\nConcluded: " + package.get("licenseConcluded", "NOASSERTION") + "\n")
        for item in value.get("hasExtractedLicensingInfos", []):
            notices.append("\n" + item.get("licenseId", "") + "\n" + item.get("extractedText", "") + "\n")
    uncovered = sorted(name for name in shipped if name.startswith(("qt6", "libqt6")) and name not in covered)
    uncovered += sorted(name for name in shipped if name.startswith("qt") and "." not in name and name not in covered)
    if uncovered:
        raise RuntimeError("Shipped Qt libraries lack SDK SBOM records: " + ", ".join(uncovered))
    if not records:
        raise RuntimeError("Qt SDK has no matching binary SBOM; cannot generate evidence")
    (target / "UPSTREAM-NOTICES.txt").write_text("".join(notices), encoding="utf-8")
    return records


def prepare(build, qt, destination, redist):
    desktop.verify_build_identity(build, "default", qt)
    problems = audit(desktop.ROOT)
    if problems:
        raise RuntimeError("Source notice verification failed: " + "; ".join(problems))
    if destination.exists() or destination.is_symlink():
        raise ValueError("Choose a new diagnostic deployment directory; existing content is never overwritten")
    evidence = destination.parent / (destination.name + "-evidence")
    if evidence.exists() or evidence.is_symlink():
        raise ValueError("Choose a new evidence directory; existing evidence is never overwritten")
    destination.mkdir(parents=True)
    evidence.mkdir(exist_ok=False)
    env = desktop.runtime_environment(desktop.environment(None), qt)
    for name in ("xdnote.exe", "xdnote_background_probe.exe", "xdnote_preview_product_probe.exe"):
        shutil.copy2(build / name, destination / name)
    execute([qt / "bin/windeployqt.exe", "--release", "--no-compiler-runtime", "--no-translations", "--no-patchqt",
             "--qmldir", desktop.ROOT / "apps/desktop/ui", "--include-plugins", "qoffscreen",
             "--exclude-plugins", "qtposition_nmea", "--dir", destination,
             destination / "xdnote.exe", destination / "xdnote_background_probe.exe", destination / "xdnote_preview_product_probe.exe"], env, evidence / "windeployqt.log", 180)
    (destination / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\nQmlImports=qml\nTranslations=translations\n", encoding="utf-8")
    system = Path(os.environ["SystemRoot"]) / "System32"
    # Only explicit official x64 CRT redistributables are admitted; no SDK or
    # foreign project's DLL search path is used to fill unresolved imports.
    redistributables = {file.name.lower(): file for file in files_under(redist) if file.suffix.lower() == ".dll"}
    for _ in range(4):
        imports, missing = dependency_closure(destination, system)
        required = {item["import"] for item in imports if item["import"] in redistributables
                    and not (destination / item["import"]).exists()}
        if not required:
            break
        for name in required:
            shutil.copy2(redistributables[name], destination / name)
    imports, missing = dependency_closure(destination, system)
    if missing:
        raise RuntimeError("Unresolved deployment imports: " + ", ".join(missing))
    licenses = destination / "licenses"
    shutil.copytree(desktop.ROOT / "LICENSES", licenses / "project-and-source-dependencies")
    shutil.copy2(desktop.ROOT / "LICENSE", licenses / "Apache-2.0.txt")
    shutil.copy2(desktop.ROOT / "THIRD_PARTY_NOTICES.md", licenses / "THIRD_PARTY_NOTICES.md")
    (licenses / "DESIGN-ATTRIBUTION.txt").write_text("文档／设计来源：xdnote 项目及其贡献者\n项目：https://github.com/xiaodaozhi/xdnote\n许可：CC BY 4.0 — https://creativecommons.org/licenses/by/4.0/\n", encoding="utf-8")
    forbidden = [path.relative_to(destination).as_posix() for path in files_under(destination)
                 if any(token in path.as_posix().lower() for token in ("webengine", "chromium", "chrome_elf", "libcef", "electron", "webview2"))]
    if forbidden:
        raise RuntimeError("Forbidden browser runtime in diagnostic staging: " + ", ".join(forbidden))
    sboms = collect_qt_notices(qt, destination)
    # Run only the diagnostic worker from the staged directory, with no SDK,
    # compiler or inherited DLL paths. Do not launch the product window.
    isolated = {name: value for name, value in os.environ.items() if not name.upper().startswith(("QT", "QML", "LD_LIBRARY", "DYLD_"))}
    isolated["PATH"] = str(destination) + os.pathsep + str(system)
    isolated = offscreen_environment(isolated, build)
    isolated.update(QT_PLUGIN_PATH=str(destination), QT_QPA_PLATFORM_PLUGIN_PATH=str(destination / "platforms"),
                    QML_IMPORT_PATH=str(destination / "qml"), QML2_IMPORT_PATH=str(destination / "qml"))
    smoke = []
    for theme in ("light", "dark"):
        output, _ = execute([destination / "xdnote_background_probe.exe", "preview", 0, theme, evidence / f"{theme}.png"],
                            isolated, evidence / f"smoke-{theme}.log", 60)
        record = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
        if len(record) != 1 or record[0].get("ok") is not True:
            raise RuntimeError("Deployed preview worker did not return successful evidence")
        smoke.append(record[0])
    output, _ = execute([destination / "xdnote_preview_product_probe.exe", 10240, "mixed", evidence / "product.png"],
                        isolated, evidence / "smoke-product.log", 90)
    product = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
    if len(product) != 1 or product[0].get("ok") is not True:
        raise RuntimeError("Deployed product file/session probe did not return successful evidence")
    inventory = [{"path": path.relative_to(destination).as_posix(), "bytes": path.stat().st_size, "sha256": digest(path)}
                 for path in files_under(destination)]
    result = {"schemaVersion": 1, "kind": "unsigned diagnostic staging; not an installer or release", "ok": True,
              "buildIdentity": json.loads((build / "build-identity.json").read_text()),
              "toolInputs": {name: digest(desktop.ROOT / name) for name in ("scripts/deployment_inventory.py", "tools/deployment-notices.json")},
              "browserRuntimeFiles": forbidden, "licenseReviewComplete": False, "files": inventory,
              "imports": imports, "missingImports": missing, "qtSbom": sboms, "offscreenSmoke": smoke, "offscreenProductSmoke": product[0],
              "diagnosticOnly": ["xdnote_background_probe.exe", "xdnote_preview_product_probe.exe", "Qt6Test.dll", "platforms/qoffscreen.dll"],
              "excludedOptionalPlugins": {"qtposition_nmea": "No NMEA/GPS serial-device feature; QtSerialPort is not a prepared product dependency"},
              "releaseReady": False, "remaining": ["review actual Qt module third-party notices and fulfill selected LGPL source/replacement obligations",
                  "MSVC redistribution terms", "minimum Windows target execution; current-host system DLL availability is not minimum-OS proof",
                  "native installer/upgrade/uninstall and file association", "signing and platform delivery policy",
                  "macOS/Linux deployment candidates", "real desktop acceptance"],
              "references": ["https://doc.qt.io/qt-6.8/windows-deployment.html", "https://doc.qt.io/qt-6.8/licensing.html",
                             "https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution"]}
    (evidence / "inventory.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"files": len(inventory), "bytes": sum(row["bytes"] for row in inventory), "missingImports": missing,
                      "qtSbomModules": len(sboms), "offscreenSmoke": len(smoke), "releaseReady": False,
                      "report": str(evidence / "inventory.json")}, ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--qt-prefix", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--redist-dir", required=True, type=Path)
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("Staging currently supports Windows only; other platforms remain explicit delivery work")
    build = desktop.build_path(args.build_dir)
    qt = desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    destination = desktop.build_path(args.destination)
    redist = desktop.checked_tool_path(args.redist_dir, "MSVC redistributables")
    prepare(build, qt, destination, redist)
