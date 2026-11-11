"""Build and verify the native semantic model; Core/Test only, no GUI process."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import desktop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cycle-native-model-admission")
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--vcvars", type=Path)
    args = parser.parse_args()
    build = desktop.build_path(args.build_dir)
    qt = desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    desktop.checked_tool_path(sys.executable, "Python interpreter")
    build.mkdir(parents=True, exist_ok=True)
    env = desktop.environment(args.vcvars)
    env["CARGO_TARGET_DIR"] = str(build / "cargo")
    env["CARGO_INCREMENTAL"] = "0"
    cmake = desktop.prepared_tool("cmake", env)
    ninja = desktop.prepared_tool("ninja", env)
    rustup = desktop.prepared_tool("rustup", env)
    cargo = [rustup, "run", desktop.DEPS["rust"]["toolchain"], "cargo"]
    rustc = subprocess.check_output([rustup, "run", desktop.DEPS["rust"]["toolchain"], "rustc", "-vV"], env=env, text=True)
    target = re.search(r"(?m)^host: (\S+)$", rustc).group(1)
    native = build / "native"
    suffix = ".exe" if platform.system() == "Windows" else ""
    library = build / "cargo" / target / "release" / ("xdnote_markdown.lib" if platform.system() == "Windows" else "libxdnote_markdown.a")
    report = {"complete": False, "scope": "read-only Rust model and Qt Core consumer; no UI/layout/resource/desktop acceptance",
              "sourceSha256": desktop.source_fingerprint(), "qtVersion": desktop.DEPS["qt"]["version"],
              "rust": rustc.strip(), "qtPrefix": str(qt), "cmake": cmake, "ninja": ninja,
              "entrySha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    project_cmake = Path(__file__).with_name("CMakeLists.txt")
    report["projectCmakeSha256"] = hashlib.sha256(project_cmake.read_bytes()).hexdigest()
    report_path = build / "model-verification.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    try:
        desktop.run(cargo + ["fmt", "--all", "--", "--check"], env, build / "fmt.log")
        desktop.run(cargo + ["clippy", "--locked", "--offline", "--workspace", "--all-targets", "--", "-D", "warnings"], env, build / "clippy.log")
        desktop.run(cargo + ["test", "--locked", "--offline", "--workspace"], env, build / "rust-test.log")
        desktop.run(cargo + ["build", "--release", "--locked", "--offline", "--target", target], env, build / "rust-build.log")
        with desktop.utf8_console():
            desktop.run([cmake, "-S", ROOT / "experiments/native-model-admission", "-B", native,
                         "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={ninja}",
                         f"-DCMAKE_PREFIX_PATH={qt}", f"-DXDNOTE_RUST_LIBRARY={library}"], env, build / "configure.log")
            desktop.run([cmake, "--build", native, "--parallel", "2"], env, build / "build.log")
        test = native / ("xdnote_native_model_tests" + suffix)
        if platform.system() == "Windows":
            dumpbin = desktop.prepared_tool("dumpbin", env)
            pending, seen, imports = [test], set(), []
            while pending:
                path = pending.pop()
                if path.name.lower() in seen:
                    continue
                seen.add(path.name.lower())
                output = subprocess.check_output([dumpbin, "/DEPENDENTS", str(path)], env=env, text=True, encoding="utf-8", errors="replace")
                for name in re.findall(r"(?im)^\s+([\w.-]+\.dll)\s*$", output):
                    if name.lower().startswith("qt6"):
                        pending.append(qt / "bin" / name)
                    imports.append(name)
            report["imports"] = sorted(set(imports))
            runtime = env.copy()
            for name in list(runtime):
                if name.startswith("QTWEBENGINE") or name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
                    runtime.pop(name)
            runtime["PATH"] = str(qt / "bin") + os.pathsep + runtime.get("PATH", "")
        else:
            command = ["otool", "-L", str(test)] if platform.system() == "Darwin" else ["ldd", str(test)]
            report["imports"] = subprocess.check_output(command, env=env, text=True).splitlines()
            runtime = env
        if any(re.search(r"webengine|webview|chromium|chrome_elf|libcef|qt6?(?:gui|widgets)", value, re.I) for value in report["imports"]):
            raise RuntimeError("The GUI-free semantic admission target imports an excluded dependency")
        qtest_report = build / "native-model-qtest.txt"
        qtest_report.unlink(missing_ok=True)
        desktop.run([test, "-o", "-,txt", "-o", f"{qtest_report},txt"], runtime, build / "native-model-run.log")
        contents = qtest_report.read_text(encoding="utf-8")
        if "0 failed, 0 skipped" not in contents or "Finished testing of NativePreviewModelTests" not in contents:
            raise RuntimeError("Native semantic tests did not produce a complete successful report")
        if re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", contents):
            raise RuntimeError("Native semantic report contains a fatal Qt diagnostic")
        if report["sourceSha256"] != desktop.source_fingerprint():
            raise RuntimeError("Source changed during verification; rerun before using this evidence")
        if (report["entrySha256"] != hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
                or report["projectCmakeSha256"] != hashlib.sha256(project_cmake.read_bytes()).hexdigest()):
            raise RuntimeError("Admission entry changed during verification")
        report["rustLibrarySha256"] = hashlib.sha256(library.read_bytes()).hexdigest()
        report["testExecutableSha256"] = hashlib.sha256(test.read_bytes()).hexdigest()
        report["complete"] = True
    finally:
        report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
