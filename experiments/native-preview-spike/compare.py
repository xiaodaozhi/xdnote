"""Windows-only, offscreen preview cost comparison; no product launch."""
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import desktop
from background_validation import offscreen_environment
from deployment_inventory import pe_imports


class ProcessEntry(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                ("name", wintypes.WCHAR * 260)]


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
    working, private, read = 0, 0, 0
    for child in ids:
        handle = kernel.OpenProcess(0x410, False, child)
        if not handle:
            continue  # A child may exit between the snapshot and query.
        try:
            counters = MemoryCounters(); counters.size = ctypes.sizeof(counters)
            if memory(handle, ctypes.byref(counters), counters.size):
                working += counters.workingSet; private += counters.private; read += 1
        finally:
            kernel.CloseHandle(handle)
    return working / 1048576, private / 1048576, read


def qt_closure(executable, qt):
    pending, seen, records = [executable], set(), []
    while pending:
        path = pending.pop()
        if path.name.lower() in seen:
            continue
        seen.add(path.name.lower())
        for name in pe_imports(path):
            if name.startswith("qt6"):
                dependency = qt / "bin" / name
                if not dependency.is_file():
                    raise RuntimeError("Missing Qt import: " + name)
                pending.append(dependency)
                if not any(row["name"] == name for row in records):
                    records.append({"name": name, "bytes": dependency.stat().st_size})
    return records


def main():
    if platform.system() != "Windows":
        raise RuntimeError("This initial process-tree sampler is Windows-only")
    build = desktop.build_path(ROOT / "build/cycle-10-preview-comparison")
    if not build.is_dir():
        raise RuntimeError("Initialize the diagnostic cycle first")
    qt = desktop.checked_tool_path(ROOT / "build/Qt/6.8.2/msvc2022_64", "Qt SDK", sdk=True)
    product = ROOT / "build/cycle-10-background"
    desktop.verify_build_identity(product, "default", qt)
    env = desktop.environment(Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    cmake, ninja = (desktop.prepared_tool(name, env) for name in ("cmake", "ninja"))
    library = product / "cargo/x86_64-pc-windows-msvc/release/xdnote_markdown.lib"
    desktop.checked_tool_path(library, "same-project Rust library")
    desktop.run([cmake, "-S", Path(__file__).parent, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_PREFIX_PATH={qt}", f"-DXDNOTE_RUST_LIBRARY={library}"], env, build / "configure.log")
    desktop.run([cmake, "--build", build, "--parallel", "2"], env, build / "build.log")
    env = offscreen_environment(desktop.runtime_environment(env, qt), build)
    report = {"scope": "isolated preview candidates; not whole-product acceptance", "qtVersion": "6.8.2", "complete": False,
              "sourceSha256": hashlib.sha256(Path(__file__).with_name("probe.cpp").read_bytes()).hexdigest(),
              "rustLibrarySha256": hashlib.sha256(library.read_bytes()).hexdigest(),
              "memoryScope": "50ms sampled complete descendant tree; private commit includes paged allocation; summed working sets may count shared pages multiple times",
              "notCovered": ["production integration", "real IME or readers", "full style fidelity", "other OS", "stable performance distribution"], "cases": []}
    try:
        for size in (10240, 1048576, 2097152):
            for kind in ("native", "webengine"):
                name = f"{kind}-{size}"
                executable = build / f"preview_{kind}.exe"
                libraries = qt_closure(executable, qt)
                if kind == "native" and any("webengine" in row["name"] for row in libraries):
                    raise RuntimeError("Native candidate unexpectedly imports WebEngine")
                with (build / f"{name}.log").open("w", encoding="utf-8") as log:
                    process = subprocess.Popen([str(executable), str(size), str(build / f"{name}.png")], cwd=ROOT, env=env,
                                               stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                    peak_working, peak_private, peak_processes, samples = 0, 0, 0, 0
                    deadline = time.monotonic() + 45
                    while process.poll() is None:
                        if time.monotonic() > deadline:
                            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
                            process.wait(timeout=15)
                            raise RuntimeError("Candidate timed out: " + name)
                        working, private, count = tree_memory(process.pid)
                        peak_working, peak_private = max(peak_working, working), max(peak_private, private)
                        peak_processes = max(peak_processes, count); samples += 1
                        time.sleep(0.05)
                if process.returncode:
                    raise RuntimeError(f"Candidate failed: {name}; exit={process.returncode}")
                output = (build / f"{name}.log").read_text(encoding="utf-8", errors="replace")
                if "QFATAL" in output or "QCRITICAL" in output:
                    raise RuntimeError("Fatal Qt diagnostic: " + name)
                rows = [json.loads(line) for line in output.splitlines() if line.startswith('{"')]
                if len(rows) != 1 or not rows[0].get("screenshot") or not samples:
                    raise RuntimeError("Missing candidate evidence: " + name)
                row = rows[0]
                row.update(peakTreePrivateCommitMiB=peak_private, peakTreeSummedWorkingSetMiB=peak_working,
                           peakObservedProcesses=peak_processes, memorySamples=samples, qtDlls=libraries,
                           qtDllBytes=sum(item["bytes"] for item in libraries))
                report["cases"].append(row)
                print(json.dumps({key: value for key, value in row.items() if key != "qtDlls"}, ensure_ascii=False), flush=True)
        for size in (10240, 1048576, 2097152):
            pair = [row for row in report["cases"] if row["sourceBytes"] == size]
            if len(pair) != 2 or len({row["sourceSha256"] for row in pair}) != 1:
                raise RuntimeError("Candidates did not receive identical source")
        report["complete"] = True
    finally:
        (build / "comparison.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    with desktop.utf8_console():
        main()
