"""Windows offscreen native-only feasibility probe. Never starts Chromium."""
import argparse
import ctypes
from ctypes import wintypes
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


class ProcessEntry(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD), ("name", wintypes.WCHAR * 260)]


class MemoryCounters(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ("peakWorkingSet", "workingSet", "peakPaged", "paged", "peakNonPaged", "nonPaged", "pagefile", "peakPagefile", "private")]


def tree_memory(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    memory = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    memory.argtypes = (wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD)
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError("Cannot enumerate diagnostic process tree")
    parents = {}
    try:
        entry = ProcessEntry(); entry.size = ctypes.sizeof(entry)
        found = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while found:
            parents[entry.pid] = entry.parent
            found = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    ids = {pid}
    while True:
        expanded = ids | {child for child, parent in parents.items() if parent in ids}
        if expanded == ids:
            break
        ids = expanded
    working, private = 0, 0
    for child in ids:
        handle = kernel.OpenProcess(0x410, False, child)
        if not handle:
            continue
        try:
            counters = MemoryCounters(); counters.size = ctypes.sizeof(counters)
            if memory(handle, ctypes.byref(counters), counters.size):
                working += counters.workingSet; private += counters.private
        finally:
            kernel.CloseHandle(handle)
    return working / 1048576, private / 1048576


def qt_closure(executable, qt, dumpbin, env):
    pending, seen, records = [executable], set(), []
    while pending:
        path = pending.pop()
        if path.name.lower() in seen:
            continue
        seen.add(path.name.lower())
        output = subprocess.check_output([dumpbin, "/DEPENDENTS", str(path)], env=env, text=True, encoding="utf-8", errors="replace")
        imports = set(re.findall(r"(?im)^\s+([\w.-]+\.dll)\s*$", output))
        for name in imports:
            if any(token in name.lower() for token in ("webengine", "webview", "chrome", "cef")):
                raise RuntimeError("Excluded browser dependency: " + name)
            if name.lower().startswith("qt6"):
                dependency = qt / "bin" / name
                if not dependency.is_file():
                    raise RuntimeError("Missing Qt import: " + name)
                pending.append(dependency)
                if not any(row["name"].lower() == name.lower() for row in records):
                    records.append({"name": name, "bytes": dependency.stat().st_size})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-build", type=Path, default=ROOT / "build/cycle-72-completion")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cycle-10-native-evaluation")
    parser.add_argument("--qt-prefix", type=Path, default=ROOT / "build/Qt/6.8.2/msvc2022_64")
    parser.add_argument("--vcvars", type=Path, default=Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("This initial memory sampler is Windows-only")
    build, source_build = desktop.build_path(args.build_dir), desktop.build_path(args.source_build)
    if not build.is_dir():
        parser.error("Initialize the dedicated build cycle before running")
    qt = desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    identity = desktop.verify_build_identity(source_build, "default", qt)
    library = source_build / "cargo/x86_64-pc-windows-msvc/release/xdnote_markdown.lib"
    desktop.checked_tool_path(library, "same-project Rust library")
    env = desktop.environment(args.vcvars)
    cmake, ninja, dumpbin = (desktop.prepared_tool(name, env) for name in ("cmake", "ninja", "dumpbin"))
    desktop.run([cmake, "-S", Path(__file__).parent, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_PREFIX_PATH={qt}", f"-DXDNOTE_RUST_LIBRARY={library}"], env, build / "configure.log")
    desktop.run([cmake, "--build", build, "--parallel", "2"], env, build / "build.log")
    executable = build / "preview_native.exe"
    libraries = qt_closure(executable, qt, dumpbin, env)
    env = {name: value for name, value in env.items() if not name.upper().startswith(("QTWEBENGINE", "QML"))}
    env.update(PATH=str(qt / "bin") + os.pathsep + env.get("PATH", ""), QT_PLUGIN_PATH=str(qt / "plugins"),
               QT_QPA_PLATFORM_PLUGIN_PATH=str(qt / "plugins/platforms"), QT_SCALE_FACTOR="1",
               QT_QPA_FONTDIR=str(Path(os.environ["SystemRoot"]) / "Fonts"))
    config = build / "offscreen.json"
    config.write_text(json.dumps({"screens": [{"name": "native-evaluation", "width": 1920, "height": 1080,
                                               "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]}), encoding="utf-8")
    env["QT_QPA_PLATFORM"] = "offscreen:configfile=" + config.relative_to(ROOT).as_posix()
    inputs = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in Path(__file__).parent.iterdir() if path.suffix in (".cpp", ".py", ".txt")}
    report = {"scope": "native-only controlled sections, Windows offscreen, not product acceptance", "complete": False,
              "sources": inputs, "rustLibrarySha256": hashlib.sha256(library.read_bytes()).hexdigest(),
              "parentBuildIdentity": identity, "qtDlls": libraries, "qtDllBytes": sum(row["bytes"] for row in libraries),
              "memoryScope": "50ms sampled full diagnostic tree; private commit may be paged; working-set sum may double-count shared pages",
              "cases": [], "notCovered": ["general AST partitioning", "within-large-block virtualization", "cross-block selection", "accessibility",
                  "production resource/link bridge", "pixel-accurate scrolling", "three-platform qualification", "total product memory"]}
    cases = [(mode, size, "dense") for size in (10240, 1048576, 2097152) for mode in ("whole", "windowed")]
    cases.extend(("windowed", 131072, flavor) for flavor in ("paragraph", "code", "table"))
    try:
        for mode, size, flavor in cases:
            name = f"{mode}-{size}-{flavor}"
            with (build / f"{name}.log").open("w", encoding="utf-8") as log:
                process = subprocess.Popen([str(executable), mode, str(size), flavor, str(build / f"{name}.png")], cwd=ROOT, env=env,
                                           stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                working, private, samples = 0, 0, 0; deadline = time.monotonic() + 45
                while process.poll() is None:
                    if time.monotonic() > deadline:
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                        process.wait(timeout=15); raise RuntimeError("Candidate timed out: " + name)
                    current_working, current_private = tree_memory(process.pid)
                    working, private = max(working, current_working), max(private, current_private); samples += 1; time.sleep(0.05)
            output = (build / f"{name}.log").read_text(encoding="utf-8", errors="replace")
            if process.returncode or re.search(r"\b(?:QFATAL|QCRITICAL)\b|ASSERT:", output):
                raise RuntimeError("Candidate failed: " + name)
            rows = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
            if len(rows) != 1 or rows[0].get("ok") is not True:
                raise RuntimeError("Missing completed evidence: " + name)
            row = rows[0]
            expected_support = flavor == "dense"
            if row.get("supported") is not expected_support:
                raise RuntimeError("Unexpected coverage result: " + name)
            row.update(peakTreePrivateCommitMiB=private, peakTreeSummedWorkingSetMiB=working, memorySamples=samples)
            report["cases"].append(row); print(json.dumps(row, ensure_ascii=False), flush=True)
        for size in (10240, 1048576, 2097152):
            pair = [row for row in report["cases"] if row["sourceBytes"] == size and row["fixture"] == "dense"]
            if len(pair) != 2 or len({row["sourceSha256"] for row in pair}) != 1:
                raise RuntimeError("Native candidates did not consume identical input")
        report["complete"] = True
    finally:
        (build / "evaluation.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    with desktop.utf8_console():
        main()
