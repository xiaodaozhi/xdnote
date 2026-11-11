"""Check recorded license inputs against locked Rust and Scintilla versions.

This detects stale/missing records and altered notice files. It does not inspect
Qt binaries, determine legal compliance, or verify a future delivery package.
"""

import hashlib
import json
from pathlib import Path, PurePosixPath
import tomllib


def validate_inventory(lock, manifest, dependencies, read_license):
    errors = []
    if manifest.get("schemaVersion") != 1:
        errors.append("Third-party license manifest has an unsupported schema")
        return errors

    def check_text(record, label):
        path = record.get("licenseFile")
        if not isinstance(path, str):
            errors.append(f"Missing license path: {label}")
            return
        parts = PurePosixPath(path).parts
        if len(parts) != 3 or parts[:2] != ("LICENSES", "third-party") or parts[2] in (".", ".."):
            errors.append(f"Invalid license path: {label}")
            return
        try:
            content = read_license(path)
        except (OSError, ValueError):
            errors.append(f"Cannot read license text: {label}")
            return
        if hashlib.sha256(content).hexdigest() != record.get("licenseSha256"):
            errors.append(f"License text digest mismatch: {label}")

    locked = {}
    for package in lock.get("package", []):
        if "source" not in package:
            continue
        key = (package.get("name"), package.get("version"))
        if (not all(isinstance(value, str) and value for value in key) or key in locked
                or not package.get("source", "").startswith("registry+") or not package.get("checksum")):
            errors.append(f"Unsupported or duplicate locked dependency: {key}")
            continue
        locked[key] = package["checksum"]

    recorded = {}
    records = manifest.get("rust")
    if not isinstance(records, list):
        errors.append("Third-party Rust records are missing")
        return errors
    for record in records:
        if not isinstance(record, dict):
            errors.append("Invalid third-party Rust record")
            continue
        key = (record.get("name"), record.get("version"))
        if not all(isinstance(value, str) and value for value in key) or key in recorded:
            errors.append(f"Invalid or duplicate third-party Rust record: {key}")
            continue
        recorded[key] = record.get("archiveSha256")
        upstream = record.get("upstreamLicense")
        selected = record.get("selectedLicense")
        if not isinstance(upstream, str) or not isinstance(selected, str) or selected not in upstream.split(" OR "):
            errors.append(f"Selected license requires review: {key}")
        check_text(record, f"{key[0]} {key[1]}")
    for key in sorted(set(locked) | set(recorded)):
        if key not in recorded:
            errors.append(f"Locked dependency has no license record: {key}")
        elif key not in locked:
            errors.append(f"License record is not in Cargo.lock: {key}")
        elif locked[key] != recorded[key]:
            errors.append(f"Locked archive digest mismatch: {key}")

    scintilla = manifest.get("scintilla")
    if not isinstance(scintilla, dict):
        errors.append("Scintilla license record is missing")
    else:
        expected = dependencies.get("scintilla", {})
        if scintilla.get("version") != expected.get("version") or scintilla.get("sourceSha256") != expected.get("sha256"):
            errors.append("Scintilla source version or digest does not match desktop dependencies")
        check_text(scintilla, "Scintilla")
    return errors


def audit(root):
    root = Path(root)
    try:
        lock = tomllib.loads((root / "Cargo.lock").read_text(encoding="utf-8"))
        manifest = json.loads((root / "LICENSES/third-party/manifest.json").read_text(encoding="utf-8"))
        dependencies = json.loads((root / "tools/desktop-dependencies.json").read_text(encoding="utf-8"))
        license_root = (root / "LICENSES/third-party").resolve(strict=True)
    except (OSError, ValueError) as error:
        return [f"Cannot read third-party license inventory: {error}"]

    def read_license(path):
        file = (root / path).resolve(strict=True)
        if not file.is_relative_to(license_root):
            raise ValueError("License path leaves its directory")
        return file.read_bytes()

    try:
        notices = json.loads((root / "tools/deployment-notices.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"Cannot read Qt notice inputs: {error}"]
    return validate_inventory(lock, manifest, dependencies, read_license) + validate_deployment_notices(notices, dependencies, read_license)


def validate_deployment_notices(notices, dependencies, read_license):
    errors = []
    if notices.get("schemaVersion") != 1 or notices.get("qtVersion") != dependencies.get("qt", {}).get("version"):
        errors.append("Qt notice version differs from locked Qt SDK")
    records = notices.get("texts", [])
    required = {"LICENSES/third-party/Qt-LGPL-3.0-only.txt", "LICENSES/third-party/Qt-GPL-3.0-only.txt"}
    if not isinstance(records, list) or {record.get("path") for record in records if isinstance(record, dict)} != required or len(records) != 2:
        errors.append("Qt LGPL and referenced GPL text inputs are missing or duplicated")
        return errors
    for record in records:
        path = record.get("path", "")
        parts = PurePosixPath(path).parts
        if len(parts) != 3 or parts[:2] != ("LICENSES", "third-party") or parts[2] in (".", ".."):
            errors.append("Qt notice path leaves recorded license directory")
            continue
        try:
            if hashlib.sha256(read_license(path)).hexdigest() != record.get("sha256"):
                errors.append(f"Qt notice digest mismatch: {path}")
        except (OSError, ValueError):
            errors.append(f"Cannot read Qt notice text: {path}")
    return errors


if __name__ == "__main__":
    import sys
    problems = audit(Path(__file__).resolve().parents[1])
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"Third-party source license inventory: {len(problems)} errors; Qt package review not included")
    sys.exit(bool(problems))
