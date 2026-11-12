"""Bounded offscreen regression and generated-sample performance evidence.

Invoked through desktop.py test --suite background so SDK and build identity
are verified before any diagnostic binary is started. No UIA or native input.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
GROUPS = {
    "pipeline": [],
    "scroll": [],
    "native_model": [],
    "native_preview": [],
    "desktop": ["systemFontsCanShapeChinese", "literalTextAndAtomicUndo", "viewLifetimeAndReadOnly", "compositionAndCommit",
                "reentrancyClosingAndWrongThread", "accessibleCommandsAndInvalidation", "accessibleWritesDoNotFlushDocumentOnAnotherThread",
                "accessibleWritesRejectUncommittedComposition", "nativeInputNotificationRejectsReentrantWrite",
                "previewIsolationAndLatestRevision", "largePreviewAndCloseInvalidateWork", "workspaceReferenceScanRetainsGapsAndDiscardsRevokedResults",
                "previewPendingHeadingRejectsContextAndRevisionChanges", "workspaceImagesUseAuthorizedSnapshotsAndStaticDecoding",
                "previewLoadsOnlyVerifiedMemoryImages", "previewDoesNotActivateLinksWhenReferenceAnalysisMayBeIncomplete"],
    "file": ["saveRoundTripAndCreateWithoutOverwrite", "conflictsFailuresAndHardLinksPreserveOriginal",
             "lateReplacementRetainsDisplacedVersionAndReportsUncertain", "changeAfterCommitNeverReportsSaved",
             "savingWhileEditingAndRepeatedOpenKeepSameSession", "reloadProtectsEditsMadeDuringReadAndRejectsOtherOpenTarget"],
    "workspace": ["transferRetainsEditorUndoSelectionAndScrollWithoutImportingParent",
                  "compositionDefersTransferAndClosedDestinationPreservesStandalone", "directVisibleWindowDestructionFlushesControllersBeforeBaseTeardown",
                  "workspaceReferencesCancelAndDiscardPreviousRoot"],
    "geometry": ["pendingResizeIsPreservedAcrossDpi", "roundTripsDoNotAccumulate", "trackingUsesHwndDpiAndPreservesUnlimitedAxes",
                 "invalidCalculationsDoNotPartiallyWrite", "fittingKeepsFrameInsideNegativeWorkArea", "insufficientLayoutIsExplicit",
                 "tiledArrangementsAreNotNormalWindows", "storeRoundTripAndMalformedPreservation", "failedPersistenceDoesNotPublishNewState"],
}
FATAL = re.compile(r"\b(?:QCRITICAL|QFATAL)\b|ASSERT failure|ASSERT:")


def offscreen_environment(environment, build):
    result = {name: value for name, value in environment.items()
              if name.upper() not in {"QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI", "QT_AUTO_SCREEN_SCALE_FACTOR",
                                       "QT_SCALE_FACTOR_ROUNDING_POLICY", "QT_DEVICE_PIXEL_RATIO", "QT_LOGGING_RULES"}}
    config = build / "offscreen-background.json"
    config.write_text(json.dumps({"screens": [{"name": "background", "width": 1920, "height": 1080,
                                               "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]}), encoding="utf-8")
    # Qt separates platform arguments on ':', so use a repository-relative path
    # rather than a Windows drive path. Never inherit the interactive QPA.
    result.update(QT_QPA_PLATFORM="offscreen:configfile=" + config.relative_to(ROOT).as_posix(),
                  QT_SCALE_FACTOR="1", QT_QUICK_BACKEND="software")
    if platform.system() == "Windows":
        result["QT_QPA_FONTDIR"] = str(Path(os.environ["SystemRoot"]) / "Fonts")
    return result


def execute(command, environment, log, timeout):
    """Kill only this diagnostic process tree on timeout; preserve all output."""
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if platform.system() == "Windows" else {"start_new_session": True}
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as output:
        process = subprocess.Popen([str(part) for part in command], cwd=ROOT, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT, **options)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            if platform.system() == "Windows":
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=15)
            raise RuntimeError(f"Diagnostic timed out after {timeout}s; see {log.name}") from None
    content = log.read_text(encoding="utf-8", errors="replace")
    if code or FATAL.search(content):
        raise RuntimeError(f"Diagnostic failed (exit={code}); see {log}")
    return content, time.monotonic() - started


def check_report(content):
    match = re.search(r"Totals:\s+(\d+) passed,\s+(\d+) failed,\s+(\d+) skipped", content)
    if not match or int(match[2]) or int(match[3]) or FATAL.search(content):
        raise RuntimeError("Offscreen QtTest must finish with zero failures/skips and no fatal Qt diagnostics")
    return int(match[1])


def cases():
    for size in (10 * 1024, 1024 * 1024, 2 * 1024 * 1024):
        yield "pipeline", size, "mixed"
        for operation in ("model", "render", "references", "headings", "session", "file"):
            yield operation, size, "mixed"
    for operation in ("model", "render", "references", "headings", "file"):
        yield operation, 10 * 1024 * 1024, "mixed"
    for operation in ("render", "references"):
        yield operation, 2 * 1024 * 1024, "dense"
    for operation in ("image", "scan", "cancel"):
        yield operation, 0, "generated"
    for theme in ("light", "dark"):
        yield "preview", 0, theme


def validate(build, environment):
    evidence = build / "evidence/background"
    evidence.mkdir(parents=True, exist_ok=True)
    budgets = json.loads((ROOT / "tools/background-budgets.json").read_text(encoding="utf-8"))
    result = {"schemaVersion": 2, "evidenceKind": "offscreen-generated-inputs", "ok": False, "guardrails": budgets,
              "buildIdentity": json.loads((build / "build-identity.json").read_text(encoding="utf-8")),
              "host": {"system": platform.system(), "release": platform.release(), "architecture": platform.machine()},
              "validationInputs": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                                   for name in ("scripts/background_validation.py", "tools/background-budgets.json")},
              "tests": {}, "measurements": [],
              "notCovered": ["real IME", "screen readers", "native file dialogs", "physical multi-monitor dragging",
                             "interactive input/scroll latency", "minimum OS", "installation", "release approval"]}
    env = offscreen_environment(environment, build)
    env["XDNOTE_TEST_EVIDENCE"] = str(evidence)
    env["XDNOTE_NATIVE_EVIDENCE"] = str(evidence)
    suffix = ".exe" if platform.system() == "Windows" else ""
    try:
        for group, functions in GROUPS.items():
            report = evidence / f"{group}-qtest.txt"
            report.unlink(missing_ok=True)
            execute([build / f"xdnote_{group}_tests{suffix}", *functions, "-o", "-,txt", "-o", f"{report},txt"],
                    env, evidence / f"{group}.log", 150)
            result["tests"][group] = {"passedIncludingFixtures": check_report(report.read_text(encoding="utf-8"))}
            print(f"Background {group}: passed", flush=True)
        for operation, size, variant in cases():
            label = f"{operation}-{size}-{variant}"
            command = [build / f"xdnote_background_probe{suffix}", operation, size, variant]
            if operation == "preview":
                command.append(evidence / f"typography-{variant}.png")
            content, wall = execute(command, env, evidence / f"{label}.log", 180 if operation == "pipeline" else 60)
            records = [json.loads(line) for line in content.splitlines() if line.startswith('{"')]
            if len(records) != 1 or records[0].get("ok") is not True:
                raise RuntimeError(f"Missing successful measurement: {label}")
            record = records[0]
            if not isinstance(record.get("peakProcessMiB"), (float, int)) or not math.isfinite(record["peakProcessMiB"]):
                raise RuntimeError(f"Invalid memory measurement: {label}")
            # Peak resident/working-set memory of this isolated worker only.
            # Resource helper children and full product costs are excluded.
            record["memoryScope"] = "isolated worker peak working set (Windows) or RSS (Unix); child processes excluded"
            record["wallSeconds"] = wall
            result["measurements"].append(record)
            if (record.get("operation"), record.get("bytes"), record.get("variant")) != (operation, size, variant):
                raise RuntimeError(f"Worker returned a different case: {label}")
            if record["peakProcessMiB"] > budgets["peakWorkerMiB"]:
                raise RuntimeError(f"Isolated worker exceeded memory guardrail: {label}")
            if operation == "pipeline":
                stages = record.get("stages", [])
                if len(stages) != 7:
                    raise RuntimeError(f"Missing pipeline phase samples: {label}")
                for sample in stages:
                    if sample["afterClose"]["entries"] or sample["afterClose"]["inFlight"]:
                        raise RuntimeError(f"Pipeline failed to release closed documents: {label}")
                    if not sample["reused"].get("analysisCacheHit") or not sample["reused"].get("projectionCacheHit"):
                        raise RuntimeError(f"Unchanged pipeline failed to reuse analysis: {label}")
                    for mode in ("cold", "reused", "burst"):
                        for name in ("semanticMs", "referencesMs", "decodeMs", "resourcesMs", "resourceReadMs", "imageDecodeMs", "projectionMs", "layoutMs", "readyMs", "queueMs", "workerMs"):
                            value = sample[mode].get(name)
                            if not isinstance(value, (float, int)) or not math.isfinite(value) or value < 0:
                                raise RuntimeError(f"Invalid pipeline phase {mode}/{name}: {label}")
            if operation == "preview":
                duration, maximum = record["preview"]["readyMs"], budgets["previewReadyMilliseconds"]
            else:
                samples = record["timing"]["samplesMs"]
                if len(samples) != 7 or any(not isinstance(value, (float, int)) or not math.isfinite(value) or value < 0 for value in samples):
                    raise RuntimeError(f"Missing or invalid measurement samples: {label}")
                duration, maximum = record["timing"]["p95Ms"], budgets["p95Milliseconds"][operation]
                if duration != max(samples):
                    raise RuntimeError(f"Incorrect nearest-rank P95: {label}")
            if not math.isfinite(duration) or duration < 0 or duration > maximum:
                raise RuntimeError(f"Offscreen timing guardrail exceeded: {label}")
            print(f"Background {label}: passed", flush=True)
        result["ok"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (evidence / "report.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Background evidence: {evidence / 'report.json'}", flush=True)
