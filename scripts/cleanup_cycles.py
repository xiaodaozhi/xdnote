"""Manage explicitly marked, disposable development-cycle directories under build/."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
MARKER = ".xdnote-disposable-cycle"
MARKER_CONTENT = "xdnote disposable cycle v1\n"


def is_link(path):
    info = path.lstat()
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def worktrees():
    data = subprocess.check_output(["git", "worktree", "list", "--porcelain", "-z"], cwd=ROOT)
    return {Path(os.fsdecode(field[9:])).resolve() for field in data.split(b"\0")
            if field.startswith(b"worktree ")}


def build_root():
    if not BUILD.is_dir() or is_link(BUILD):
        raise ValueError("build must be an existing ordinary directory")
    return BUILD.resolve(strict=True)


def candidate(name, active):
    root = build_root()
    if not name.startswith("cycle-") or name in ("cycle-", ".", "..") or Path(name).name != name:
        raise ValueError("Use the name of a direct build/cycle-* directory")
    path = root / name
    if not path.is_dir() or is_link(path):
        raise ValueError("Cycle must be an ordinary directory")
    path = path.resolve(strict=True)
    if path.parent != root:
        raise ValueError("Cycle escapes build")
    if any(path == item or item.is_relative_to(path) for item in active):
        raise ValueError("Cycle contains an active worktree")
    marker = path / MARKER
    if not marker.is_file() or is_link(marker) or marker.read_text(encoding="utf-8") != MARKER_CONTENT:
        raise ValueError("Cycle does not have the expected ownership marker")
    return path


def newest(path):
    latest = path.lstat().st_mtime
    for directory, dirs, files in os.walk(path, followlinks=False, onerror=lambda e: (_ for _ in ()).throw(e)):
        for name in dirs + files:
            item = Path(directory) / name
            if is_link(item):
                raise ValueError("Cycle contains a link or reparse point")
            latest = max(latest, item.lstat().st_mtime)
    return latest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    select = parser.add_mutually_exclusive_group(required=True)
    select.add_argument("--init", metavar="CYCLE_NAME", help="Create a new empty cycle-* directory")
    select.add_argument("--path", metavar="CYCLE_NAME", help="Select one marked cycle-* directory")
    select.add_argument("--stale-days", type=int, help="Select marked cycles with no recent changes")
    parser.add_argument("--apply", action="store_true", help="Remove selected directories after validation")
    parser.add_argument("--yes", action="store_true", help="Confirm removal")
    args = parser.parse_args()
    if args.apply != args.yes or (args.init and args.apply):
        parser.error("Removal requires --apply and --yes together; --init cannot remove")
    try:
        if args.init and not BUILD.exists():
            BUILD.mkdir()
        root = build_root()
        if args.init:
            name = args.init
            if not name.startswith("cycle-") or name == "cycle-" or Path(name).name != name:
                raise ValueError("Use a direct cycle-* name")
            path = root / name
            path.mkdir()  # Never adopt existing data.
            (path / MARKER).write_text(MARKER_CONTENT, encoding="utf-8")
            print(f"Created build/{name}")
            return
        if args.stale_days is not None and args.stale_days <= 0:
            raise ValueError("--stale-days must be positive")
        active = worktrees()
        names = [args.path] if args.path else sorted(p.name for p in root.iterdir() if p.name.startswith("cycle-"))
        selected = []
        for name in names:
            try:
                path = candidate(name, active)
                modified = newest(path)
                if args.stale_days is None or modified <= time.time() - args.stale_days * 86400:
                    selected.append(path)
            except (ValueError, OSError) as exc:
                if args.path:
                    raise
                print(f"Skipped build/{name}: {exc}")
        for path in selected:
            # Recheck safety immediately before each removal.
            print(f"{'Remove' if args.apply else 'Preview'} build/{path.name}")
            if args.apply:
                candidate(path.name, worktrees())
                modified = newest(path)
                if args.stale_days is not None and modified > time.time() - args.stale_days * 86400:
                    raise ValueError("Cycle changed after selection")
                shutil.rmtree(path)
        print(f"Selected: {len(selected)}")
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Cleanup refused: {exc}\n")


if __name__ == "__main__":
    main()
