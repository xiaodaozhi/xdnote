#!/usr/bin/env python3
"""Explicitly prepare the pinned Scintilla source under the ignored build tree."""

import argparse
import hashlib
import io
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request

URL = "https://www.scintilla.org/scite567.tgz"
SHA256 = "ad31f6e89aefe9c4e2c48f64c720f311e6a43369288e7b109d90b507ca623198"
MAX_ARCHIVE = 16 * 1024 * 1024
MAX_SOURCE = 64 * 1024 * 1024


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--download", action="store_true", help="explicitly download the pinned archive")
    source.add_argument("--archive", type=pathlib.Path, help="use an existing archive without downloading")
    args = parser.parse_args()

    if args.download:
        request = urllib.request.Request(URL, headers={"User-Agent": "xdnote-scintilla-experiment"})
        with urllib.request.urlopen(request, timeout=60) as response:
            archive = response.read(MAX_ARCHIVE + 1)
    else:
        if args.archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError("Archive exceeds size limit")
        archive = args.archive.read_bytes()
    if len(archive) > MAX_ARCHIVE or hashlib.sha256(archive).hexdigest() != SHA256:
        raise ValueError("Scintilla archive size or SHA-256 does not match the pinned input")

    root = pathlib.Path(__file__).resolve().parents[2]
    build = root / "build"
    if build.is_symlink():
        raise ValueError("The build directory must not be a symlink")
    build.mkdir(exist_ok=True)
    build = build.resolve()
    if not build.is_relative_to(root.resolve()):
        raise ValueError("Build directory escapes the repository")
    destination = build / "scintilla-5.6.7"
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"Destination already exists: {destination}")

    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as package:
        members = [m for m in package.getmembers() if m.name.startswith("scintilla/")]
        if not members or sum(m.size for m in members if m.isfile()) > MAX_SOURCE:
            raise ValueError("Archive has no Scintilla source or exceeds the source size limit")
        for member in members:
            path = pathlib.PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or ":" in member.name or not (member.isfile() or member.isdir())):
                raise ValueError(f"Unsafe archive entry: {member.name}")

        # Extract only ordinary files into a newly created, verified build subdirectory.
        with tempfile.TemporaryDirectory(prefix="scintilla-prepare-", dir=build) as temporary:
            temporary_path = pathlib.Path(temporary).resolve()
            if not temporary_path.is_relative_to(build) or not destination.parent.resolve() == build:
                raise ValueError("Temporary or destination path escapes the build directory")
            for member in members:
                target = temporary_path.joinpath(*pathlib.PurePosixPath(member.name).parts)
                if not target.resolve().is_relative_to(temporary_path):
                    raise ValueError(f"Archive entry escapes destination: {member.name}")
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.extractfile(member) as source_file, target.open("xb") as output:
                        shutil.copyfileobj(source_file, output)
            if destination.exists() or destination.is_symlink():
                raise FileExistsError(f"Destination appeared during preparation: {destination}")
            temporary_path.rename(destination)
    print(f"Prepared Scintilla 5.6.7 source in {destination}")


if __name__ == "__main__":
    main()
