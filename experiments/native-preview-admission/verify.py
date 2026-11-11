"""Build native Widgets admission and run offscreen tests without the product."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import desktop
sys.path.insert(0, str(ROOT / "experiments/native-preview-spike"))
from evaluate import qt_closure, tree_memory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cycle-104-native-layout")
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--vcvars", type=Path)
    parser.add_argument("--full", action="store_true", help="also run Rust fmt/clippy/tests")
    parser.add_argument("--measure", action="store_true", help="Windows process-tree samples of owned fixtures")
    args = parser.parse_args()
    build = desktop.build_path(args.build_dir)
    qt = desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    desktop.checked_tool_path(sys.executable, "Python interpreter")
    build.mkdir(parents=True, exist_ok=True)
    env = desktop.environment(args.vcvars)
    env["CARGO_TARGET_DIR"] = str(build / "cargo"); env["CARGO_INCREMENTAL"] = "0"
    cmake, ninja, rustup = [desktop.prepared_tool(name, env) for name in ("cmake", "ninja", "rustup")]
    cargo = [rustup, "run", desktop.DEPS["rust"]["toolchain"], "cargo"]
    rustc = subprocess.check_output([rustup, "run", desktop.DEPS["rust"]["toolchain"], "rustc", "-vV"], env=env, text=True)
    target = re.search(r"(?m)^host: (\S+)$", rustc).group(1)
    library = build / "cargo" / target / "release" / ("xdnote_markdown.lib" if platform.system() == "Windows" else "libxdnote_markdown.a")
    native = build / "native"
    report = {"complete": False, "scope": "native Widgets admission, not product or real desktop acceptance",
              "sourceSha256": desktop.source_fingerprint(), "rust": rustc.strip(), "qtPrefix": str(qt),
              "entrySha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "fullRustChecks": args.full}
    report["admissionInputsSha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in Path(__file__).parent.iterdir() if path.is_file() and path.suffix in (".cpp", ".txt", ".py")}
    report_path = build / "native-verification.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    try:
        if args.full:
            desktop.run(cargo + ["fmt", "--all", "--", "--check"], env, build / "fmt.log")
            desktop.run(cargo + ["clippy", "--workspace", "--all-targets", "--locked", "--offline", "--", "-D", "warnings"], env, build / "clippy.log")
            desktop.run(cargo + ["test", "--workspace", "--locked", "--offline"], env, build / "rust-test.log")
        desktop.run(cargo + ["build", "--release", "--locked", "--offline", "--target", target], env, build / "rust-build.log")
        with desktop.utf8_console():
            desktop.run([cmake, "-S", ROOT / "experiments/native-preview-admission", "-B", native, "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_PREFIX_PATH={qt}",
                         f"-DXDNOTE_RUST_LIBRARY={library}"], env, build / "configure.log")
            desktop.run([cmake, "--build", native, "--parallel", "2"], env, build / "build.log")
        runtime = env.copy()
        for name in list(runtime):
            if name.startswith("QTWEBENGINE") or name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
                runtime.pop(name)
        runtime["QT_QPA_PLATFORM"] = "offscreen"
        runtime["XDNOTE_NATIVE_EVIDENCE"] = str(build / "evidence")
        runtime["QT_PLUGIN_PATH"] = str(qt / "plugins")
        runtime["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt / "plugins/platforms")
        if platform.system() == "Windows":
            runtime["PATH"] = str(qt / "bin") + os.pathsep + runtime.get("PATH", "")
            runtime["QT_QPA_FONTDIR"] = str(Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts")
        suffix = ".exe" if platform.system() == "Windows" else ""
        test = native / ("xdnote_native_preview_tests" + suffix)
        if platform.system() == "Windows":
            report["qtImports"] = qt_closure(test, qt, desktop.prepared_tool("dumpbin", env), env)
        for executable, filename, class_name in ((test, "native-preview-qtest.txt", "NativePreviewViewTests"),
                (native / ("xdnote_native_model_tests" + suffix), "native-model-qtest.txt", "NativePreviewModelTests")):
            path = build / filename; path.unlink(missing_ok=True)
            desktop.run([executable, "-o", "-,txt", "-o", f"{path},txt"], runtime, build / (filename + ".log"))
            contents = path.read_text(encoding="utf-8")
            if ("0 failed, 0 skipped" not in contents or f"Finished testing of {class_name}" not in contents
                    or re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", contents)):
                raise RuntimeError("Incomplete or failed native report: " + filename)
        if args.measure:
            if platform.system() != "Windows":
                raise RuntimeError("This process-tree sampler is Windows-only; no platform results are inferred")
            probe = native / ("xdnote_native_preview_probe" + suffix)
            report["probeQtImports"] = qt_closure(probe, qt, desktop.prepared_tool("dumpbin", env), env)
            samples = []
            for flavor, size in [("idle", 10240), ("dense", 10240), ("dense", 1048576), ("dense", 2097152),
                                 ("paragraph", 2097152), ("code", 2097152), ("table", 2097152), ("many", 2097152), ("bidi", 2097152), ("cluster", 2097152)]:
                name = f"{flavor}-{size}"
                log = build / (name + ".log")
                print("Measuring native fixture:", name, flush=True)
                private, working, count = 0, 0, 0
                with log.open("w", encoding="utf-8") as output:
                    child = subprocess.Popen([str(probe), str(size), flavor, str(build / "evidence" / (name + ".png"))],
                                             cwd=ROOT, env=runtime, stdout=output, stderr=subprocess.STDOUT)
                    while child.poll() is None:
                        resident, committed = tree_memory(child.pid)
                        private, working, count = max(private, committed), max(working, resident), count + 1
                        time.sleep(.05)
                contents = log.read_text(encoding="utf-8")
                rows = [json.loads(line) for line in contents.splitlines() if line.startswith("{")]
                if (child.returncode != 0 or len(rows) != 1 or not rows[0].get("ok")
                        or re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", contents)):
                    raise RuntimeError("Native fixture failed: " + name + "; see " + str(log))
                sample = rows[0]
                sample.update(sampledPeakTreePrivateMiB=private, sampledPeakTreeWorkingSetMiB=working,
                              samples=count, samplePeriodMs=50)
                samples.append(sample)
                report["samples"] = samples
                report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
            report["probeSha256"] = hashlib.sha256(probe.read_bytes()).hexdigest()
        if report["sourceSha256"] != desktop.source_fingerprint():
            raise RuntimeError("Source changed during validation")
        for name, digest in report["admissionInputsSha256"].items():
            if hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest() != digest:
                raise RuntimeError("Admission input changed during validation: " + name)
        report["librarySha256"] = hashlib.sha256(library.read_bytes()).hexdigest()
        report["testSha256"] = hashlib.sha256(test.read_bytes()).hexdigest()
        report["complete"] = True
    finally:
        report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
