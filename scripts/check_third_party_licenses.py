"""Check recorded license inputs against locked Rust and Scintilla versions.

This detects stale/missing records and altered notice files. It does not inspect
Qt binaries, determine legal compliance, or verify a future delivery package.
"""

import hashlib
import json
from pathlib import Path, PurePosixPath
import tomllib
import re


DEPLOYMENT_NOTICE_PATHS = frozenset({
    "LICENSES/third-party/Qt-LGPL-3.0-only.txt",
    "LICENSES/third-party/Qt-GPL-3.0-only.txt",
    "LICENSES/third-party/Qt-PSL-MPL-2.0.txt",
    "LICENSES/third-party/Qt-Tika-NOTICE.txt",
    "LICENSES/third-party/Qt-Tika-OGL-UK-3.0.txt",
    "LICENSES/third-party/QtGui-QIMAGETRANSFORM-LICENSE.txt",
    "LICENSES/third-party/QtSvg-XSVG-LICENSE.txt",
})


def license_choices(expression):
    """Expand the recorded SPDX AND/OR choices; reject unsupported syntax.

    The legacy MIT/Apache-2.0 spelling occurs in locked crate metadata. WITH
    stays attached to its license and cannot be dropped from a selected option.
    This is a record consistency check, not a legal interpretation.
    """
    if not isinstance(expression, str):
        raise ValueError("Missing license expression")
    expression = expression.replace("MIT/Apache-2.0", "(MIT OR Apache-2.0)")
    tokens = re.findall(r"[A-Za-z0-9.+-]+|[()]", expression)
    if "".join(tokens) != re.sub(r"\s", "", expression):
        raise ValueError("Unsupported license syntax")
    index = 0

    def atom():
        nonlocal index
        if index >= len(tokens):
            raise ValueError("Missing license")
        value = tokens[index]; index += 1
        if value == '(':
            result = choices()
            if index >= len(tokens) or tokens[index] != ')':
                raise ValueError("Unclosed license expression")
            index += 1
            return result
        if value in {'AND', 'OR', 'WITH', ')'}:
            raise ValueError("Invalid license")
        if index < len(tokens) and tokens[index] == 'WITH':
            index += 1
            if index >= len(tokens) or tokens[index] in {'AND', 'OR', 'WITH', '(', ')'}:
                raise ValueError("Missing license exception")
            value += ' WITH ' + tokens[index]; index += 1
        return {frozenset({value})}

    def conjunction():
        nonlocal index
        result = atom()
        while index < len(tokens) and tokens[index] == 'AND':
            index += 1
            right = atom()
            result = {left | other for left in result for other in right}
        return result

    def choices():
        nonlocal index
        result = conjunction()
        while index < len(tokens) and tokens[index] == 'OR':
            index += 1
            result |= conjunction()
        return result

    result = choices()
    if index != len(tokens):
        raise ValueError("Trailing license syntax")
    return result


def source_license_texts(manifest):
    """All recorded primary and additional source notices required in a payload."""
    result = []
    for record in [*manifest['rust'], manifest['scintilla']]:
        result.append(record)
        result.extend(record.get('additionalTexts', []))
    return result


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
        try:
            selection = license_choices(selected)
            valid_choice = len(selection) == 1 and selection <= license_choices(upstream)
        except (ValueError, RecursionError):
            valid_choice = False
        if not valid_choice:
            errors.append(f"Selected license requires review: {key}")
        check_text(record, f"{key[0]} {key[1]}")
        additional = record.get("additionalTexts", [])
        if not isinstance(additional, list):
            errors.append(f"Invalid additional license texts: {key}")
        else:
            for text in additional:
                if not isinstance(text, dict):
                    errors.append(f"Invalid additional license text: {key}")
                else:
                    check_text(text, f"{key[0]} {key[1]} additional text")
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
    if (not isinstance(records, list) or len(records) != len(DEPLOYMENT_NOTICE_PATHS)
            or {record.get("path") for record in records if isinstance(record, dict)} != DEPLOYMENT_NOTICE_PATHS):
        errors.append("Qt distribution notice inputs are missing, unknown, or duplicated")
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
