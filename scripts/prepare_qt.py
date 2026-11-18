"""Explicit, checksum-preserving Qt SDK preparation for desktop admission."""

import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download", action="store_true", required=True)
    parser.add_argument("--host", choices=("windows", "linux", "mac"), required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--profile", choices=("default", "msvc2019"), default="default")
    parser.add_argument("--vcvars", type=Path)
    parser.add_argument("--supplier-only", action="store_true", help="unmodified SDK for read-only historical supplier evidence")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "tools/desktop-dependencies.json").read_text(encoding="utf-8"))
    qt = manifest["qt"]
    version = qt["version"]
    if args.profile != "default":
        compatibility = manifest["compatibilityProfiles"][args.profile]
        if compatibility["host"] != args.host:
            parser.error("The selected compatibility profile is Windows-only")
    if importlib.metadata.version("aqtinstall") != qt["aqtinstall"]:
        parser.error("Install the pinned aqtinstall version in an isolated tooling environment first")
    allowed = {"windows": "win64_msvc2022_64", "linux": "linux_gcc_64", "mac": "clang_64"}
    if args.arch != allowed[args.host]:
        parser.error("Architecture is outside this admission SDK matrix")

    # Use aqt's supported metadata layout and retain XML and binary checksums.
    from aqt.installer import Cli
    import qt_network_backports
    build = ROOT / "build"
    if build.is_symlink() or (hasattr(build, "is_junction") and build.is_junction()):
        parser.error("Build directory cannot be a symlink")
    build.mkdir(exist_ok=True)
    if not build.resolve().is_relative_to(ROOT.resolve()):
        parser.error("Build directory escapes the repository")
    folder = {"windows": "msvc2022_64", "linux": "gcc_64", "mac": "macos"}[args.host]
    prefix = build / "Qt" / version / folder
    if (prefix / qt_network_backports.RECEIPT).exists():
        if args.supplier_only:
            parser.error("Supplier-only evidence requires an unmodified SDK directory")
        qt_network_backports.verify(prefix)
        return 0
    os.chdir(build)
    archives_to_install = ["qtbase", "qtdeclarative", "qttools", "qtshadertools", "qtsvg"]
    if args.host == "linux":
        # Official Linux Qt binaries require the SDK's matching ICU libraries.
        # The minimum reference system ships an older incompatible ICU SONAME.
        # Wayland is a separate SDK archive; keep both supported native backends.
        archives_to_install.extend(("icu", "qtwayland"))
    result = Cli().run([
        "install-qt", args.host, "desktop", version, args.arch,
        "-O", str(build / "Qt"), "-m", *qt["modules"],
        "--archives", *archives_to_install,
    ])
    if result:
        return result
    os.chdir(ROOT)
    if args.supplier_only:
        return 0
    output = build / "QtNetworkBackports" / qt_network_backports.config()["variant"] / folder
    qt_network_backports.build(prefix, output, allow_download=True, vcvars=args.vcvars)
    return 0


if __name__ == "__main__":
    sys.exit(main())
