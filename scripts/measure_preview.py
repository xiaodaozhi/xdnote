"""Measure only owned offscreen product fixtures, including Qt dependency closure."""
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

import desktop

ROOT = desktop.ROOT
sys.path.insert(0, str(ROOT / "experiments/native-preview-spike"))
from evaluate import qt_closure, tree_memory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--vcvars", type=Path)
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("This process-tree sampler is Windows-only")
    build = desktop.build_path(args.build_dir)
    qt = desktop.checked_tool_path(args.qt_prefix, "Qt SDK", sdk=True)
    identity = desktop.verify_build_identity(build, "default", qt)
    env = desktop.runtime_environment(desktop.environment(args.vcvars), qt)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QT_QPA_FONTDIR"] = str(Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts")
    dumpbin = desktop.prepared_tool("dumpbin", env)
    probe = build / "xdnote_preview_product_probe.exe"
    evidence = build / "product-memory"; evidence.mkdir(exist_ok=True)
    report = {"complete": False, "sourceSha256": identity["sourceSha256"],
              "probeSha256": hashlib.sha256(probe.read_bytes()).hexdigest(),
              "productSha256": hashlib.sha256((build / "xdnote.exe").read_bytes()).hexdigest(),
              "productQtImports": qt_closure(build / "xdnote.exe", qt, dumpbin, env),
              "probeQtImports": qt_closure(probe, qt, dumpbin, env), "samples": [],
              "scope": "owned actual standalone window/file/session fixtures; offscreen, not real desktop or release budget"}
    path = build / "product-memory.json"
    try:
        for flavor, size in [("idle", 10240), ("dense", 10240), ("dense", 2097152), ("paragraph", 2097152), ("table", 2097152)]:
            name = f"{flavor}-{size}"; log = evidence / (name + ".log")
            print("Measuring product fixture:", name, flush=True)
            private, working, samples = 0, 0, 0; start = time.monotonic()
            with log.open("w", encoding="utf-8") as output:
                child = subprocess.Popen([str(probe), str(size), flavor, str(evidence / (name + ".png"))], env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
                try:
                    while child.poll() is None:
                        if time.monotonic() - start > 90:
                            raise RuntimeError("Owned fixture timed out: " + name)
                        resident, committed = tree_memory(child.pid)
                        working, private, samples = max(working, resident), max(private, committed), samples + 1
                        time.sleep(.05)
                finally:
                    if child.poll() is None:
                        child.kill(); child.wait(timeout=10)
            text = log.read_text(encoding="utf-8")
            rows = [json.loads(line) for line in text.splitlines() if line.startswith("{")]
            if child.returncode != 0 or len(rows) != 1 or not rows[0].get("ok") or re.search(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:", text):
                raise RuntimeError(f"Product fixture failed (exit {child.returncode}): {log}")
            sample = rows[0]; sample.update(sampledPeakTreePrivateMiB=private, sampledPeakTreeWorkingSetMiB=working, samples=samples, samplePeriodMs=50)
            report["samples"].append(sample)
        desktop.verify_build_identity(build, "default", qt)
        report["complete"] = True
    finally:
        path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()
