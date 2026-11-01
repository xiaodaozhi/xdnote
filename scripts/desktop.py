#!/usr/bin/env python3
"""One explicit prepare/build/test/run entry point for desktop admission."""

import argparse
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


def environment(vcvars):
    env = os.environ.copy()
    if vcvars:
        path = Path(vcvars).resolve(strict=True)
        if platform.system() != "Windows" or '"' in str(path):
            raise ValueError("--vcvars requires an ordinary Windows vcvars64.bat path")
        # Import toolchain variables without printing the machine environment.
        output = subprocess.check_output(f'cmd.exe /d /u /s /c ""{path}" >nul && set"',
                                         text=True, encoding="utf-16-le")
        for line in output.splitlines():
            name, separator, value = line.partition("=")
            if separator and name:
                env[name] = value
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("doctor", "prepare", "build", "test", "run"))
    parser.add_argument("--download", action="store_true", help="authorize network access in prepare only")
    parser.add_argument("--qt-prefix", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--vcvars", type=Path)
    parser.add_argument("--build-dir", type=Path, default=BUILD / "desktop-admission")
    parser.add_argument("--parallel", type=int, default=2)
    args = parser.parse_args()
    build = build_path(args.build_dir)
    BUILD.mkdir(exist_ok=True)
    env = environment(args.vcvars)
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
    for tool in (args.cmake, args.ninja, "rustup"):
        if not shutil.which(str(tool), path=env.get("PATH")):
            raise RuntimeError(f"Missing prepared tool: {tool}")
    run([args.cmake, "--version"], env)
    run(["rustup", "run", DEPS["rust"]["toolchain"], "rustc", "-vV"], env)
    if not args.qt_prefix:
        arch = {"Windows": "msvc2022_64", "Darwin": "macos", "Linux": "gcc_64"}[platform.system()]
        args.qt_prefix = BUILD / "Qt" / DEPS["qt"]["version"] / arch
    qt = args.qt_prefix.resolve(strict=True)
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
        print(f"Qt prefix: {qt}\nScintilla: {BUILD / 'scintilla-5.6.7'}\nBuild: {build}")
        return
    build.mkdir(parents=True, exist_ok=True)
    if args.command == "build":
        run([args.cmake, "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_PREFIX_PATH={qt}",
             f"-DPython3_EXECUTABLE={sys.executable}"], env, build / "configure.log")
        run([args.cmake, "--build", build, "--parallel", str(args.parallel)], env, build / "build.log")
        (build / "build-identity.json").write_text(json.dumps({
            "sourceSha256": source_fingerprint(), "dependencies": DEPS,
            "platform": platform.system(), "architecture": platform.machine(),
        }, indent=2) + "\n", encoding="utf-8")
    elif args.command == "test":
        identity = json.loads((build / "build-identity.json").read_text(encoding="utf-8"))
        if identity["sourceSha256"] != source_fingerprint():
            raise RuntimeError("Sources changed since the successful build; rebuild before testing")
        run(cargo("fmt", "--all", "--", "--check"), env, build / "rust-fmt.log")
        run(cargo("clippy", "--locked", "--offline", "--workspace", "--all-targets", "--", "-D", "warnings"), env, build / "rust-clippy.log")
        run(cargo("test", "--locked", "--offline", "--workspace"), env, build / "rust-test.log")
        evidence = build / "evidence"
        evidence.mkdir(exist_ok=True)
        env["XDNOTE_TEST_EVIDENCE"] = str(evidence)
        ctest = str(Path(args.cmake).with_name("ctest" + (".exe" if platform.system() == "Windows" else ""))) if Path(args.cmake).is_absolute() else "ctest"
        # Verbose output retains Qt diagnostics even when QtTest itself passes.
        native_report = build / "desktop-qtest.txt"
        native_report.unlink(missing_ok=True)
        try:
            run([ctest, "--test-dir", build, "--verbose", "--no-tests=error"], env, build / "desktop-test.log")
        finally:
            if not native_report.is_file():
                raise RuntimeError("This test run did not create its QtTest report")
            report_text = native_report.read_text(encoding="utf-8", errors="replace")
            print(report_text, flush=True)
            if re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", report_text):
                raise RuntimeError("The native QtTest report contains a fatal diagnostic")
    else:
        executable = build / ("xdnote-admission.exe" if platform.system() == "Windows" else "xdnote-admission")
        run([executable], env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Desktop admission: {error}", file=sys.stderr)
        sys.exit(1)
