"""User-scope developer installation with an owned, hashed file manifest.

No defaults takeover, signing, release publication, or user-document cleanup.
The CLI prints a plan unless --apply is supplied. Payloads must already contain
their runtime closure; deployment_inventory.py / deployment_unix.py prepare it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import uuid

MANIFEST = ".xdnote-install.json"
OWNER = "xiaodaozhi/xdnote/user-install-v1"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(folder):
    folder = Path(folder)
    if folder.is_symlink() or folder.is_junction() or not folder.is_dir():
        raise ValueError("Payload or installation must be a real directory")
    result = {}
    for path in folder.rglob("*"):
        if path.is_junction():
            raise ValueError("Directory junctions are not allowed in an installation")
        if path.is_symlink():
            if Path(os.readlink(path)).is_absolute() or not path.resolve().is_relative_to(folder.resolve()):
                raise ValueError("Installation link escapes its payload")
            result[path.relative_to(folder).as_posix()] = "link:" + os.readlink(path)
            continue
        if path.is_file() and path != folder / MANIFEST:
            result[path.relative_to(folder).as_posix()] = digest(path)
    return result


def checked_installation(folder):
    folder = Path(folder)
    manifest = folder / MANIFEST
    if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("Destination has no owned installation manifest")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("owner") != OWNER or data.get("schemaVersion") != 1 or data.get("files") != inventory(folder):
        raise ValueError("Installation contains unknown or changed files; preserved for inspection")
    return data


def executable(folder, system):
    folder = Path(folder)
    if system == "Windows":
        return folder / "xdnote.exe"
    if system == "Darwin":
        return folder / "xdnote.app/Contents/MacOS/xdnote"
    return folder / "xdnote" if (folder / "xdnote").is_file() else folder / "bin/xdnote"


def registration_plan(exe):
    exe = str(exe)
    if any(c in exe for c in '\r\n"'):
        raise ValueError("Executable path cannot be represented safely")
    command = '"' + exe + '" "%1"'
    classes = "Software\\Classes\\"
    return [
        (classes + "xdnote.Markdown", "", "Markdown 文档"),
        (classes + "xdnote.Markdown\\DefaultIcon", "", '"' + exe + '",0'),
        (classes + "xdnote.Markdown\\shell\\open\\command", "", command),
        (classes + ".md\\OpenWithProgids", "xdnote.Markdown", ""),
        (classes + ".markdown\\OpenWithProgids", "xdnote.Markdown", ""),
        (classes + "Applications\\xdnote.exe\\shell\\open\\command", "", command),
        (classes + "Applications\\xdnote.exe\\SupportedTypes", ".md", ""),
        (classes + "Applications\\xdnote.exe\\SupportedTypes", ".markdown", ""),
        ("Software\\xdnote\\Capabilities", "ApplicationName", "xdnote"),
        ("Software\\xdnote\\Capabilities", "ApplicationDescription", "Markdown 阅读与编辑"),
        ("Software\\xdnote\\Capabilities\\FileAssociations", ".md", "xdnote.Markdown"),
        ("Software\\xdnote\\Capabilities\\FileAssociations", ".markdown", "xdnote.Markdown"),
        ("Software\\RegisteredApplications", "xdnote", "Software\\xdnote\\Capabilities"),
    ]


def windows_registration(exe, remove=False):
    import winreg
    entries = registration_plan(exe)
    if remove:
        # Another installation may have replaced the shared ProgID meanwhile.
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, entries[2][0]) as key:
                if winreg.QueryValueEx(key, "")[0] != entries[2][2]:
                    return
        except FileNotFoundError:
            return
    for path, name, value in reversed(entries) if remove else entries:
        if remove:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE) as key:
                    if winreg.QueryValueEx(key, name)[0] == value:
                        winreg.DeleteValue(key, name)
            except FileNotFoundError:
                pass
        else:
            with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_WRITE) as key:
                winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
    if remove:
        # Remove only empty keys; never recursively delete shared registration.
        for path, _, _ in sorted(entries, key=lambda e: len(e[0]), reverse=True):
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
            except OSError:
                pass
    import ctypes
    ctypes.windll.shell32.SHChangeNotify(0x08000000, 0, None, None)


def desktop_entry(exe):
    # Desktop Entry Exec quoting has a separate escape grammar, not shell syntax.
    text = str(exe)
    if "\n" in text or "\r" in text:
        raise ValueError("Invalid desktop executable path")
    text = text.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%")
    return "[Desktop Entry]\nType=Application\nName=xdnote\nComment=Markdown reader and editor\nExec=\"" + text + "\" %F\nTerminal=false\nCategories=Office;TextEditor;\nMimeType=text/markdown;text/x-markdown;\n"


def integrate(folder, system, remove=False, data_home=None):
    exe = executable(folder, system)
    if system == "Windows":
        windows_registration(exe, remove)
    elif system == "Linux":
        root = Path(data_home or os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
        path = root / "applications/io.github.xiaodaozhi.xdnote.desktop"
        expected = desktop_entry(exe)
        if path.is_symlink() or (path.exists() and path.read_text(encoding="utf-8") != expected):
            raise ValueError("Existing desktop entry differs; it was preserved")
        if remove:
            if path.exists():
                path.unlink()
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(expected, encoding="utf-8")
    # macOS document declarations live in the bundle; Launch Services discovers
    # the app when the user opens it. Do not force a global handler preference.


def remove_owned(folder):
    folder = Path(folder)
    checked_installation(folder)
    files = [p for p in folder.rglob("*") if p.is_file() or p.is_symlink()]
    for path in files:
        path.unlink()
    for path in sorted((p for p in folder.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        path.rmdir()
    folder.rmdir()


def install(payload, destination, system=None, apply=False, registration=False, data_home=None):
    system = system or platform.system()
    payload, destination = Path(payload).absolute(), Path(destination).absolute()
    if destination.is_symlink() or destination.is_junction() or any(p.is_symlink() or p.is_junction() for p in destination.parents):
        raise ValueError("Linked installation destination is not allowed")
    if payload.is_symlink() or payload.is_junction():
        raise ValueError("Payload root must be a real directory")
    payload, destination = payload.resolve(), destination.resolve()
    if payload == destination or payload in destination.parents or destination in payload.parents:
        raise ValueError("Payload and destination must be separate directories")
    files = inventory(payload)
    if not executable(payload, system).is_file() or MANIFEST in files or not files:
        raise ValueError("Payload is not a prepared xdnote application")
    if (payload / MANIFEST).exists():
        raise ValueError("An installed directory cannot be reused as a payload")
    if destination.exists():
        checked_installation(destination)
    result = {"operation": "upgrade" if destination.exists() else "install", "destination": str(destination), "files": len(files), "apply": apply, "registration": registration, "releaseReady": False}
    if not apply:
        return result
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = destination.parent / (".xdnote-stage-" + uuid.uuid4().hex)
    backup = destination.parent / (".xdnote-backup-" + uuid.uuid4().hex)
    stage.mkdir()
    try:
        for relative in files:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            source = payload / relative
            if source.is_symlink():
                target.symlink_to(os.readlink(source), target_is_directory=source.is_dir())
            else:
                shutil.copy2(source, target)
        if inventory(stage) != files:
            raise ValueError("Copied payload failed its hash verification")
        (stage / MANIFEST).write_text(json.dumps({"schemaVersion": 1, "owner": OWNER, "files": files, "system": system}, ensure_ascii=False, indent=2), encoding="utf-8")
        if destination.exists():
            destination.rename(backup)
        try:
            stage.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise
        if backup.exists():
            remove_owned(backup)
    finally:
        if stage.exists():
            # Stage was created by this invocation; no caller-supplied deletion.
            if stage.parent.resolve() != destination.parent.resolve() or not stage.name.startswith(".xdnote-stage-"):
                raise ValueError("Unsafe staging cleanup target")
            shutil.rmtree(stage)
    if registration:
        try:
            integrate(destination, system, data_home=data_home)
            result["integrationComplete"] = True
        except (OSError, ValueError) as error:
            result["integrationComplete"] = False
            result["integrationError"] = str(error)
    return result


def uninstall(destination, apply=False, registration=False, data_home=None):
    destination = Path(destination).absolute()
    if destination.is_symlink() or destination.is_junction() or any(p.is_symlink() or p.is_junction() for p in destination.parents):
        raise ValueError("Linked installation destination is not allowed")
    destination = destination.resolve()
    manifest = checked_installation(destination)
    result = {"operation": "uninstall", "destination": str(destination), "apply": apply, "preservesUserData": True}
    if apply:
        if registration:
            integrate(destination, manifest["system"], remove=True, data_home=data_home)
        remove_owned(destination)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("install", "uninstall"))
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--register", action="store_true", help="User-scope open-with registration; never sets defaults")
    args = parser.parse_args()
    if args.operation == "install" and not args.payload:
        parser.error("install requires --payload")
    result = install(args.payload, args.destination, apply=args.apply, registration=args.register) if args.operation == "install" else uninstall(args.destination, args.apply, args.register)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result.get("integrationError"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
