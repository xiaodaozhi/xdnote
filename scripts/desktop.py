#!/usr/bin/env python3
"""One explicit prepare/build/test/run entry point for desktop admission."""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DEPS = json.loads((ROOT / "tools/desktop-dependencies.json").read_text(encoding="utf-8"))
SYSTEM_BREW_PREFIXES = tuple(Path(value) for value in ("/opt/homebrew", "/usr/local", "/home/linuxbrew/.linuxbrew"))


def repository_owner(path):
    """Return the nearest checkout and its common Git identity, including worktrees."""
    path = Path(path).resolve(strict=True)
    for directory in (path, *path.parents):
        marker = directory / ".git"
        if marker.is_dir():
            return directory, marker.resolve()
        if marker.is_file():
            value = marker.read_text(encoding="utf-8").strip()
            if not value.startswith("gitdir: "):
                raise ValueError("Invalid Git checkout marker in a tool path")
            gitdir = (directory / value[8:]).resolve(strict=True)
            common = gitdir / "commondir"
            identity = (gitdir / common.read_text(encoding="utf-8").strip()).resolve(strict=True) if common.is_file() else gitdir
            return directory, identity
    return None


def system_package_tool(path, owner):
    # Homebrew itself is a Git checkout. Only installed Cellar packages with a
    # receipt are system tools; its arbitrary source checkouts are not exempt.
    prefix = owner[0]
    if prefix not in SYSTEM_BREW_PREFIXES or not (prefix / "Library/Homebrew/brew.sh").is_file():
        return False
    cellar = prefix / "Cellar"
    if not path.is_relative_to(cellar):
        return False
    parts = path.relative_to(cellar).parts
    return len(parts) >= 3 and (cellar / parts[0] / parts[1] / "INSTALL_RECEIPT.json").is_file()


def checked_tool_path(value, label, sdk=False):
    path = Path(value).resolve(strict=True)
    owner, current = repository_owner(path), repository_owner(ROOT)
    if owner and (not current or owner[1] != current[1]) and (sdk or not system_package_tool(path, owner)):
        raise ValueError(f"{label} belongs to another Git repository; prepare tools in this project's build/toolchain")
    if sdk and not path.is_relative_to((owner[0] if owner else ROOT) / "build"):
        raise ValueError(f"{label} must be inside this project's build tree")
    return path


def prepared_tool(value, env):
    # A virtual environment's own launchers take priority over inherited PATH.
    sibling = Path(sys.executable).parent / (value + (".exe" if platform.system() == "Windows" else ""))
    candidate = str(sibling) if value in ("cmake", "ninja") and sibling.is_file() else str(value)
    found = shutil.which(candidate, path=env.get("PATH"))
    if not found:
        raise RuntimeError(f"Missing prepared tool: {value}")
    return str(checked_tool_path(found, str(value)))


@contextlib.contextmanager
def utf8_console():
    if platform.system() != "Windows":
        yield
        return
    import ctypes
    console = ctypes.windll.kernel32
    input_page, output_page = console.GetConsoleCP(), console.GetConsoleOutputCP()
    if not input_page or not output_page:
        yield
        return
    try:
        # CMake decodes /showIncludes using the input page, while cl writes
        # using the output page. Keep both UTF-8 for the command, then restore.
        if not console.SetConsoleCP(65001) or not console.SetConsoleOutputCP(65001):
            raise OSError("Cannot select UTF-8 for this build console")
        yield
    finally:
        console.SetConsoleCP(input_page)
        console.SetConsoleOutputCP(output_page)


def build_path(value):
    path = Path(value).resolve()
    if path == BUILD.resolve() or not path.is_relative_to(BUILD.resolve()):
        raise ValueError("Choose a dedicated directory inside this repository's build tree")
    current = Path(value).absolute()
    while current != ROOT.parent:
        if current.is_symlink() or (hasattr(current, "is_junction") and current.is_junction()):
            raise ValueError("Build paths cannot contain links or junctions")
        current = current.parent
    return path


def run(command, env, log=None):
    print("+", " ".join(str(part) for part in command), flush=True)
    process = subprocess.Popen([str(part) for part in command], cwd=ROOT, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace")
    fatal = False
    with process.stdout, (log.open("w", encoding="utf-8") if log else open(os.devnull, "w")) as report:
        for line in process.stdout:
            print(line, end="", flush=True)
            report.write(line)
            fatal |= bool(re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", line))
    code = process.wait()
    if code or fatal:
        raise RuntimeError(f"Command failed (exit={code}, Qt fatal diagnostic={fatal})")


def profile_settings(profile, system=None):
    if profile == "default":
        return {"qtVersion": DEPS["qt"]["version"], "compatibilityOnly": False}
    config = DEPS["compatibilityProfiles"].get(profile)
    if not config or (system or platform.system()) != "Windows":
        raise ValueError("Unknown or non-Windows compatibility profile")
    return {"qtVersion": DEPS["qt"]["version"], "compatibilityOnly": True}


def find_msvc2019():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    for versions in ("[16.0,17.0)", "[17.0,18.0)"):
        install = subprocess.check_output([
            str(vswhere), "-latest", "-products", "*", "-version", versions, "-utf8",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath",
        ], text=True, encoding="utf-8").strip()
        if install and list((Path(install) / "VC/Tools/MSVC").glob("14.29.*/bin/Hostx64/x64/cl.exe")):
            return Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    raise RuntimeError("Installed MSVC v142 14.29 tools not found; provide --vcvars explicitly")


def environment(vcvars, toolset=None):
    env = ({name.upper(): value for name, value in os.environ.items()}
           if platform.system() == "Windows" else os.environ.copy())
    if vcvars:
        path = checked_tool_path(vcvars, "MSVC environment script")
        if platform.system() != "Windows" or '"' in str(path):
            raise ValueError("--vcvars requires an ordinary Windows vcvars64.bat path")
        # Import toolchain variables without printing the machine environment.
        if toolset and not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", toolset):
            raise ValueError("Invalid MSVC toolset version")
        option = f" -vcvars_ver={toolset}" if toolset else ""
        output = subprocess.check_output(f'cmd.exe /d /u /s /c ""{path}"{option} >nul && set"',
                                         text=True, encoding="utf-16-le")
        for line in output.splitlines():
            name, separator, value = line.partition("=")
            if separator and name:
                # Windows names are case-insensitive. A plain dict otherwise keeps
                # both PATH and Path, so child DLL lookup can ignore our Qt prefix.
                env[name.upper()] = value
    env["CARGO_HOME"] = str(BUILD / "cargo-home")
    env["CARGO_TARGET_DIR"] = str(BUILD / "cargo-admission")
    env["RUSTUP_AUTO_INSTALL"] = "0"
    return env


def cargo(*args):
    return ["rustup", "run", DEPS["rust"]["toolchain"], "cargo", *args]


def source_fingerprint():
    paths = [ROOT / name for name in ("CMakeLists.txt", "Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "docs/visual/tokens.json")]
    for folder in ("apps/desktop", "crates", "cmake", "tools"):
        paths.extend(path for path in (ROOT / folder).rglob("*") if path.is_file())
    vendor = BUILD / f"scintilla-{DEPS['scintilla']['version']}" / "scintilla"
    paths.extend(path for path in vendor.rglob("*") if path.suffix in (".h", ".cxx", ".cpp"))
    paths.extend(ROOT / "scripts" / name for name in ("desktop.py", "prepare_qt.py"))
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(path.relative_to(ROOT).as_posix().encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def verify_build_identity(build, profile, qt):
    identity = json.loads((build / "build-identity.json").read_text(encoding="utf-8"))
    if identity.get("profile") != profile or os.path.normcase(identity.get("qtPrefix", "")) != os.path.normcase(str(qt)):
        raise RuntimeError("Build profile or Qt SDK differs from the successful build; rebuild in its own directory")
    if identity["sourceSha256"] != source_fingerprint():
        raise RuntimeError("Sources changed since the successful build; rebuild before testing or running")
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("doctor", "prepare", "build", "test", "run"))
    parser.add_argument("--download", action="store_true", help="authorize network access in prepare only")
    parser.add_argument("--qt-prefix", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--vcvars", type=Path)
    parser.add_argument("--vcvars-version", help="explicit installed MSVC toolset, for example 14.29")
    parser.add_argument("--profile", choices=("default", "msvc2019"), default="default")
    parser.add_argument("--file", type=Path, action="append", default=[], help="local Markdown file to open with run; repeat for multiple files")
    parser.add_argument("--admission", action="store_true", help="run the memory-only component window instead of xdnote")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--parallel", type=int, default=2)
    parser.add_argument("--fresh", action="store_true", help="recreate CMake configuration in the validated build directory")
    args = parser.parse_args()
    checked_tool_path(sys.executable, "Python interpreter")
    checked_tool_path(getattr(sys, "_base_executable", sys.executable), "Python base interpreter")
    if args.fresh and args.command != "build":
        parser.error("--fresh is supported by build only")
    if (args.file or args.admission) and args.command != "run":
        parser.error("--file and --admission are supported by run only")
    if args.file and args.admission:
        parser.error("The admission window does not accept user files")
    settings = profile_settings(args.profile)
    build = build_path(args.build_dir or BUILD / ("desktop-admission" if args.profile == "default" else "desktop-msvc2019"))
    BUILD.mkdir(exist_ok=True)
    if args.profile == "msvc2019" and args.command != "prepare" and not args.vcvars:
        args.vcvars = find_msvc2019()
    env = environment(args.vcvars, args.vcvars_version or ("14.29" if args.profile == "msvc2019" else None))
    if args.command != "run":
        prepared_tool("rustup", env)
    if args.profile != "default":
        env["CARGO_TARGET_DIR"] = str(BUILD / f"cargo-{args.profile}")
    if args.command == "prepare":
        if not args.download:
            parser.error("prepare requires --download; build and test never download dependencies")
        scintilla = BUILD / f"scintilla-{DEPS['scintilla']['version']}"
        if not scintilla.exists():
            run([sys.executable, ROOT / "experiments/scintilla-buffer-spike/prepare.py", "--download"], env)
        run(cargo("fetch", "--locked", "--manifest-path", ROOT / "Cargo.toml"), env)
        return
    if args.download:
        parser.error("--download is supported by prepare only")
    if args.command != "run":
        args.cmake = prepared_tool(args.cmake, env)
        args.ninja = prepared_tool(args.ninja, env)
        run([args.cmake, "--version"], env)
        run(["rustup", "run", DEPS["rust"]["toolchain"], "rustc", "-vV"], env)
    if not args.qt_prefix:
        arch = {"Windows": "msvc2022_64", "Darwin": "macos", "Linux": "gcc_64"}[platform.system()]
        args.qt_prefix = BUILD / "Qt" / settings["qtVersion"] / arch
    qt = checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    if not (qt / "lib/cmake/Qt6WebEngineWidgets/Qt6WebEngineWidgetsConfig.cmake").is_file():
        raise RuntimeError("Qt WebEngine SDK missing; run the explicit SDK preparation first")
    env["PATH"] = str(qt / "bin") + os.pathsep + env["PATH"]
    env["QML_IMPORT_PATH"] = str(qt / "qml")
    env["QT_PLUGIN_PATH"] = str(qt / "plugins")
    if platform.system() == "Linux":
        env["LD_LIBRARY_PATH"] = str(qt / "lib") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        for binary in (qt / "plugins/platforms/libqxcb.so", qt / "libexec/QtWebEngineProcess"):
            libraries = subprocess.check_output(["ldd", str(binary)], env=env, text=True)
            missing = [line.strip() for line in libraries.splitlines() if "not found" in line]
            if missing:
                raise RuntimeError(f"Runtime dependencies missing for {binary.name}: {'; '.join(missing)}")
    env["CARGO_NET_OFFLINE"] = "true"
    if args.command == "doctor":
        print(f"Profile: {args.profile}\nQt {settings['qtVersion']}: {qt}\nScintilla: {BUILD / 'scintilla-5.6.7'}\nBuild: {build}")
        return
    build.mkdir(parents=True, exist_ok=True)
    if args.command == "build":
        run([args.cmake, *(["--fresh"] if args.fresh else []), "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_PREFIX_PATH={qt}",
             f"-DXDNOTE_BUILD_PROFILE={args.profile}",
             f"-DPython3_EXECUTABLE={sys.executable}"], env, build / "configure.log")
        run([args.cmake, "--build", build, "--parallel", str(args.parallel)], env, build / "build.log")
        (build / "build-identity.json").write_text(json.dumps({
            "sourceSha256": source_fingerprint(), "dependencies": DEPS,
            "platform": platform.system(), "architecture": platform.machine(),
            "profile": args.profile, "qtPrefix": str(qt), "compatibilityOnly": settings["compatibilityOnly"],
            "nativeToolchain": json.loads((build / "native-toolchain.json").read_text(encoding="utf-8")),
        }, indent=2) + "\n", encoding="utf-8")
    elif args.command == "test":
        verify_build_identity(build, args.profile, qt)
        run(cargo("fmt", "--all", "--", "--check"), env, build / "rust-fmt.log")
        run(cargo("clippy", "--locked", "--offline", "--workspace", "--all-targets", "--", "-D", "warnings"), env, build / "rust-clippy.log")
        run(cargo("test", "--locked", "--offline", "--workspace"), env, build / "rust-test.log")
        evidence = build / "evidence"
        evidence.mkdir(exist_ok=True)
        env["XDNOTE_TEST_EVIDENCE"] = str(evidence)
        ctest = str(Path(args.cmake).with_name("ctest" + (".exe" if platform.system() == "Windows" else ""))) if Path(args.cmake).is_absolute() else "ctest"
        # Verbose output retains Qt diagnostics even when QtTest itself passes.
        native_reports = [build / name for name in ("desktop-qtest.txt", "file-qtest.txt", "visual-qtest.txt", "geometry-qtest.txt", "workspace-qtest.txt")]
        for report in native_reports:
            report.unlink(missing_ok=True)
        try:
            run([ctest, "--test-dir", build, "--verbose", "--no-tests=error"], env, build / "desktop-test.log")
        finally:
            for native_report in native_reports:
                if not native_report.is_file():
                    raise RuntimeError(f"This test run did not create {native_report.name}")
                report_text = native_report.read_text(encoding="utf-8", errors="replace")
                print(report_text, flush=True)
                if re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", report_text):
                    raise RuntimeError("The native QtTest report contains a fatal diagnostic")
    else:
        verify_build_identity(build, args.profile, qt)
        name = "xdnote-admission" if args.admission else "xdnote"
        executable = build / (name + (".exe" if platform.system() == "Windows" else ""))
        run([executable, *(path.absolute() for path in args.file)], env)


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    try:
        with utf8_console():
            main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Desktop admission: {error}", file=sys.stderr)
        sys.exit(1)
