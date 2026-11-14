"""Measure isolated native Windows application processes, without desktop input."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
import zlib
import struct

import desktop
from installer_common import ROOT, build_output, digest


def fixture(root, scenario):
    root.mkdir(parents=True, exist_ok=False)
    (root / 'fixture.json').write_text(json.dumps(scenario), encoding='utf-8')
    # An original, deterministic PNG; no network images or user files.
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 640, 360, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress((b'\x00' + b'\x70\x80\x90' * 640) * 360)) + chunk(b'IEND', b'')
    (root / 'assets/images').mkdir(parents=True)
    (root / 'assets/images/sample.png').write_bytes(png)
    section = ('\n## 中文阅读与编辑\n\n让思考慢慢清晰。Unicode café 😀\n\n'
               '> 少一点干扰，多一点专注。\n\n- [x] 记录新的想法\n- [ ] 完成编辑\n\n'
               '```cpp\nconst auto title = "xdnote";\n```\n\n'
               '| 项目 | 状态 |\n| --- | --- |\n| 中文表格 | 就绪 |\n\n')
    for index in range(scenario['documents']):
        text = '# 文档 %d\n\n![本地图片](assets/images/sample.png)\n' % index
        # Complete semantic blocks: source bytes are close to and never above the configured cap.
        while len((text + section).encode('utf-8')) <= scenario['bytesPerDocument']:
            text += section
        (root / f'document-{index}.md').write_text(text, encoding='utf-8', newline='\n')
    return [dict(path=p.name, bytes=p.stat().st_size, sha256=digest(p)) for p in sorted(root.glob('*.md'))]


def runtime_environment(qt, payload=None):
    environment = {key: value for key, value in os.environ.items()
                   if not key.upper().startswith(('QT', 'QML', 'LD_LIBRARY', 'DYLD_'))}
    if payload:
        environment.update(PATH=str(payload) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32'),
                           QT_PLUGIN_PATH=str(payload), QT_QPA_PLATFORM_PLUGIN_PATH=str(payload / 'platforms'),
                           QML_IMPORT_PATH=str(payload / 'qml'), QML2_IMPORT_PATH=str(payload / 'qml'))
    else:
        environment = desktop.runtime_environment(environment, qt)
    environment['QT_QPA_PLATFORM'] = 'windows'
    environment['QT_FORCE_STDERR_LOGGING'] = '1'
    environment['QT_MESSAGE_PATTERN'] = '%{if-critical}QCRITICAL %{endif}%{if-fatal}QFATAL %{endif}%{type}: %{message}'
    return environment


def run_sample(executable, qt, output, scenario, payload=None):
    original = fixture(output, scenario)
    ready, result = output / 'ready.json', output / 'result.json'
    started = time.perf_counter()
    with (output / 'process.log').open('w', encoding='utf-8') as log:
        child = subprocess.Popen([str(executable), scenario['name'], str(output), str(ready), str(result)],
                                 env=runtime_environment(qt, payload), stdout=log, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        observed = None
        try:
            while child.poll() is None and time.perf_counter() - started < 100:
                if ready.is_file() and observed is None:
                    observed = (time.perf_counter() - started) * 1000
                time.sleep(0.01)
            code = child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill(); child.wait()
            raise RuntimeError('Owned performance probe exceeded its deadline')
    log = (output / 'process.log').read_text(encoding='utf-8', errors='replace')
    if code or not result.is_file() or observed is None or any(x in log for x in ('QCRITICAL', 'QFATAL', 'ASSERT failure', 'ASSERT:')):
        raise RuntimeError(f'Probe failed; inspect {output / "process.log"} and result.json')
    value = json.loads(result.read_text(encoding='utf-8'))
    if not value.get('ok'):
        raise RuntimeError(f'Probe rejected scenario: {value}')
    expected_runtime = payload if payload else qt / 'bin'
    if Path(value['qtCorePath']).resolve().parent != expected_runtime.resolve():
        raise RuntimeError('Probe loaded Qt from outside the selected runtime')
    value.update(processStartToReadyObservedMs=observed, fixtureBefore=original,
                 readinessPollIntervalMs=10, executableSha256=digest(executable))
    (output / 'sample.json').write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return value


def summarize(samples):
    def summary(values):
        ordered = sorted(values)
        return {'median': statistics.median(ordered), 'p95NearestRank': ordered[math.ceil(len(ordered) * .95) - 1],
                'min': ordered[0], 'max': ordered[-1], 'samples': len(ordered)}
    return {'processStartToReadyObservedMs': summary([s['processStartToReadyObservedMs'] for s in samples]),
            **{name: summary([s['idleMemorySamples'][-1][name] for s in samples])
               for name in ('workingSetBytes', 'privateCommitBytes', 'peakWorkingSetBytes')},
            'editToSettledPreviewMs': summary([v for s in samples for v in s['editToSettledPreviewMs']])
            if any(s['editToSettledPreviewMs'] for s in samples) else None}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'qt-prefix', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--payload', type=Path, help='Use only this sealed product runtime; probe remains outside the payload')
    p.add_argument('--repeats', type=int)
    p.add_argument('--scenario', choices=('empty', 'reading', 'editing', 'split', 'multitab'))
    a = p.parse_args()
    a.build_dir, a.qt_prefix = a.build_dir.resolve(), a.qt_prefix.resolve()
    if platform.system() != 'Windows':
        p.error('This baseline currently collects Windows native processes only')
    desktop.verify_build_identity(a.build_dir, 'default', a.qt_prefix)
    output = build_output(a.output)
    if output.exists():
        p.error('Use a new evidence directory')
    config = json.loads((ROOT / 'tools/app-performance.json').read_text())
    repeats = a.repeats if a.repeats is not None else config['repeats']
    if not 1 <= repeats <= 10:
        p.error('Use 1..10 independent processes per scenario')
    output.mkdir(parents=True)
    report = {'schemaVersion': 1, 'platform': platform.platform(), 'logicalCpus': os.cpu_count(),
              'buildIdentity': json.loads((a.build_dir / 'build-identity.json').read_text()),
              'scenarioConfigSha256': digest(ROOT / 'tools/app-performance.json'), 'repeats': repeats,
              'scope': 'hidden native Qt windows and real application services; warm OS cache; no user settings or desktop input',
              'exclusions': ['visible compositor presentation', 'IME and real input latency', 'other processes',
                             'cold disk/cache conditions', 'installer/signing/lowest-OS acceptance'],
              'budget': 'measurement baseline only; no whole-application budget has been approved', 'scenarios': {}}
    fixture_font = os.environ.get('XDNOTE_TEST_FONT_FILE')
    report['diagnosticFont'] = {'file': Path(fixture_font).name, 'sha256': digest(Path(fixture_font))} if fixture_font else None
    for scenario in config['scenarios']:
        if a.scenario and scenario['name'] != a.scenario:
            continue
        values = []
        for index in range(repeats):
            print(f'{scenario["name"]} {index + 1}/{repeats}', flush=True)
            values.append(run_sample(a.build_dir.resolve() / 'xdnote_app_probe.exe', a.qt_prefix.resolve(),
                                     output / f'{scenario["name"]}-{index + 1}', scenario,
                                     a.payload.resolve() if a.payload else None))
        report['scenarios'][scenario['name']] = {'summary': summarize(values), 'raw': values}
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Application baseline recorded: ' + str(output / 'report.json'))


if __name__ == '__main__':
    main()
