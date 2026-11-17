"""Real Windows/Linux product update roundtrips in disposable Actions accounts.

Private worktree instrumentation injects owned metadata/package replies, an
ephemeral compiled trust root, silent Windows Setup and headless Linux sudo.
Ordinary product sources, trust files, SDKs and personal installations stay intact.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import time
import desktop
from check_native_update import replace, dump, checked
from check_update_helper import guard, command, wait_file
from installer_common import ROOT, build_output, digest
from release_candidate import TARGETS

TEMPLATES = ROOT / 'experiments/native-update-fixture'


class Product:
    def __init__(self, work, qt, toolchain, runtime, vcvars):
        self.work, self.qt, self.windows = work, qt, platform.system() == 'Windows'
        self.env = desktop.environment(vcvars); self.env['CARGO_NET_OFFLINE'] = 'true'
        self.source = work / 'source'; self.run = self.source / 'build/native-run'
        if command(['git', '-C', ROOT, 'status', '--porcelain']).stdout.strip(): raise ValueError('Commit fixture inputs first')
        self.revision = command(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.decode().strip()
        checked(['git', '-C', ROOT, 'worktree', 'add', '--detach', self.source, self.revision], log=work / 'worktree.log')
        self.run.mkdir(parents=True)
        if desktop.repository_owner(self.source)[1] != desktop.repository_owner(ROOT)[1]: raise ValueError('Different project identity')
        shutil.copytree(ROOT / 'build/scintilla-5.6.7', self.source / 'build/scintilla-5.6.7')
        if self.windows:
            self.toolchain = self.source / 'build/toolchain/installer'; self.runtime = self.source / 'build/toolchain/windows-runtime'
            shutil.copytree(toolchain, self.toolchain); shutil.copytree(runtime, self.runtime)
        replace(self.source / 'scripts/desktop.py', 'str(BUILD / "cargo-home")', repr(str(ROOT / 'build/cargo-home')))
        replace(self.source / 'CMakeLists.txt', '${PROJECT_SOURCE_DIR}/build/cargo-home', (ROOT / 'build/cargo-home').as_posix())
        fixture = self.source / 'experiments/native-update-fixture'
        (fixture / 'ProductFixture.cpp').write_text((TEMPLATES / 'ProductFixture.cpp.in').read_text().replace('@DIRECTORY@', json.dumps(str(self.run))), encoding='utf-8')
        (fixture / 'Fixture.h').write_text('#pragma once\n#include <QString>\nclass QApplication;class QNetworkAccessManager;class QObject;\nQString fixtureDirectory();QNetworkAccessManager *fixtureNetwork(QObject *);void fixtureBoot(QApplication *);void fixtureHelper(qint64,const QString &,const QString &);\n')
        replace(self.source / 'CMakeLists.txt', 'qt_add_executable(xdnote WIN32 apps/desktop/main.cpp)', 'target_sources(xdnote_desktop PRIVATE experiments/native-update-fixture/ProductFixture.cpp)\ntarget_include_directories(xdnote_desktop PUBLIC experiments/native-update-fixture)\nqt_add_executable(xdnote WIN32 apps/desktop/main.cpp)')
        replace(self.source / 'apps/desktop/main.cpp', '#include "BuildInfo.h"', '#include "BuildInfo.h"\n#include "Fixture.h"')
        replace(self.source / 'apps/desktop/main.cpp', '    if (!installation.allowed()) {', '    fixtureBoot(&application); return application.exec();\n    if (!installation.allowed()) {')
        controller = self.source / 'apps/desktop/src/UpdateController.cpp'
        replace(controller, '#include "UpdateDownload.h"', '#include "UpdateDownload.h"\n#include "Fixture.h"')
        replace(controller, ': UpdateController(nullptr, BuildInfo::version()', ': UpdateController(fixtureNetwork(parent), BuildInfo::version()')
        replace(controller, 'm_attemptPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates/attempt.json";', 'm_attemptPath = fixtureDirectory() + "/attempt.json";')
        handoff = self.source / 'apps/desktop/src/UpdateHandoff.cpp'
        replace(handoff, '#include "UpdateHandoff.h"', '#include "UpdateHandoff.h"\n#include "Fixture.h"')
        replace(handoff, '    m_processId = pid;', '    m_processId = pid; fixtureHelper(pid, m_directory, m_transaction);')
        if self.windows:
            replace(self.source / 'crates/update-helper/src/windows.rs', '.arg("/NORESTART")', '.arg("/VERYSILENT").arg("/SUPPRESSMSGBOXES").arg("/NORESTART")')
        else:
            replace(handoff, 'program = QStringLiteral("/usr/libexec/xdnote-update-helper");', 'program = QStringLiteral("/usr/bin/sudo");')
            replace(handoff, 'arguments = {QStringLiteral("--authorize"), requestFile};', 'arguments = {QStringLiteral("-n"), QStringLiteral("env"), QStringLiteral("PKEXEC_UID=' + str(os.getuid()) + '"), QStringLiteral("/usr/libexec/xdnote-update-helper"), QStringLiteral("--request"), requestFile};')
            replace(self.source / 'crates/update-helper/src/linux.rs', '                "DISPLAY",', '                "QT_QPA_PLATFORM",\n                "QT_QPA_PLATFORM_PLUGIN_PATH",\n                "DISPLAY",')
        self.patch_launcher()
        examples = self.source / 'crates/update/examples'; examples.mkdir(exist_ok=True)
        shutil.copy2(TEMPLATES / 'fixture_sign.rs.in', examples / 'fixture_sign.rs')
        self.seed = self.run / 'private-seed'; self.seed.write_bytes(os.urandom(32)); self.seed.chmod(0o600)
        signer_target = work / 'signer'
        checked(['rustup', 'run', '1.99.0', 'cargo', 'build', '--locked', '--offline', '--manifest-path', self.source / 'Cargo.toml', '-p', 'xdnote-update', '--example', 'fixture_sign', '--target-dir', signer_target], env=self.env, log=work / 'signer.log')
        self.signer = signer_target / ('debug/examples/fixture_sign.exe' if self.windows else 'debug/examples/fixture_sign')
        public = checked([self.signer, 'public', self.seed]).stdout.decode().strip()
        dump(self.source / 'tools/update-trust.json', {'schema_version': 1, 'keys': [{'key_id': 'isolated-native-fixture', 'public_key': public}]})
        checked(['git', '-C', self.source, 'add', 'CMakeLists.txt', 'apps/desktop', 'crates/update-helper', 'crates/update/examples', 'experiments/native-update-fixture', 'scripts/desktop.py', 'tools/update-trust.json'])
        checked(['git', '-C', self.source, '-c', 'user.name=xdnote CI Fixture', '-c', 'user.email=fixture@invalid.example', '-c', 'commit.gpgsign=false', 'commit', '-m', 'test: isolate real product update fixture'])
        self.identities, self.packages = {}, {}
        for label, version, build, previous, previous_build in [('old', '0.1.0-alpha.1', 200, '0.0.0', 0), ('new', '0.1.0-alpha.2', 201, '0.1.0-alpha.1', 200)]:
            if label == 'new': checked(['git', '-C', self.source, '-c', 'user.name=xdnote CI Fixture', '-c', 'user.email=fixture@invalid.example', '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', 'test: identify private product target 201'])
            revision = checked(['git', '-C', self.source, 'rev-parse', 'HEAD']).stdout.decode().strip()
            identity = self.source / ('build/' + label + '-identity.json')
            dump(identity, {'schemaVersion': 1, 'product': 'xdnote', 'version': version, 'build': build, 'revision': revision, 'previousVersion': previous, 'previousBuild': previous_build})
            args = [sys.executable, self.source / 'scripts/desktop.py', 'build', '--profile', 'default', '--build-dir', self.source / 'build/desktop', '--qt-prefix', qt, '--release-identity', identity, '--parallel', '4']
            if vcvars: args += ['--vcvars', vcvars]
            checked(args, env=self.env, log=work / (label + '-build.log'))
            self.identities[label] = json.loads((self.source / 'build/desktop/generated/build-version.json').read_bytes())
            self.packages[label] = self.package(label)
        self.candidate(); self.seed.unlink()

    def patch_launcher(self):
        launcher = self.source / 'scripts/desktop.py'
        replace(launcher, '    parser.add_argument("--fresh",', '    parser.add_argument("--fixture-installation", type=Path)\n    parser.add_argument("--fresh",')
        replace(launcher, '        run([executable, *(path.absolute() for path in args.file)], env)', '''        if args.fixture_installation:
            root = args.fixture_installation.absolute()
            marker = json.loads((BUILD / 'native-run/allowed.json').read_text())
            executable = root / ('xdnote.exe' if platform.system() == 'Windows' else 'bin/xdnote')
            if marker.get('owner') != 'xdnote-private-native-product-v1' or marker.get('root') != str(root) or hashlib.sha256(executable.read_bytes()).hexdigest() != marker.get('executableSha256'):
                raise ValueError('Private product fixture installation differs')
            env['QT_QPA_PLATFORM'] = 'offscreen'
            env['QT_PLUGIN_PATH'] = str(root if platform.system() == 'Windows' else root / 'plugins')
            env['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(qt / 'plugins/platforms')
            if platform.system() == 'Windows': env['PATH'] = str(root) + os.pathsep + env.get('PATH', '')
            else: env['LD_LIBRARY_PATH'] = str(root / 'lib')
        run([executable, *(path.absolute() for path in args.file)], env)''')

    def tool(self, script, args, log):
        checked([sys.executable, self.source / 'scripts' / script, *args], env=self.env, log=self.work / log)

    def package(self, label):
        build = self.source / 'build/desktop'; base = self.source / ('build/' + label)
        if self.windows:
            staging = base.with_name(label + '-staging'); payload = base.with_name(label + '-payload'); audit = base.with_name(label + '-payload.json')
            self.tool('deployment_inventory.py', ['--build-dir', build, '--qt-prefix', self.qt, '--destination', staging, '--redist-dir', self.runtime / 'runtime'], label + '-deploy.log')
            self.tool('installer_payload.py', ['--source', staging, '--audit', str(staging) + '-evidence/inventory.json', '--destination', payload, '--toolchain', self.toolchain, '--runtime', self.runtime / 'runtime', '--build-dir', build, '--qt-prefix', self.qt, '--report', audit], label + '-payload.log')
            package = base.with_suffix('.exe')
            self.tool('build-windows-installer.py', ['--root', payload, '--audit', audit, '--toolchain', self.toolchain, '--output', package, '--report', base.with_suffix('.json')], label + '-package.log')
        else:
            payload = base.with_name(label + '-product')
            self.tool('product_deployment_unix.py', ['--build-dir', build, '--qt-prefix', self.qt, '--destination', payload], label + '-deploy.log')
            package = base.with_suffix('.deb')
            self.tool('build_linux_installer.py', ['--root', payload, '--audit', str(payload) + '-evidence/inventory.json', '--output', package, '--report', base.with_suffix('.json')], label + '-package.log')
        return package

    def candidate(self):
        identity = self.identities['new']; version = identity['version']; package = self.packages['new']
        suffix = 'windows-x86_64-setup.exe' if self.windows else 'ubuntu22.04-x86_64.deb'
        self.filename = 'xdnote-' + version + '-' + suffix
        base = 'https://github.com/xiaodaozhi/xdnote/releases/download/v' + version + '/'
        self.routes = {'https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=1': 'listing.json', base + 'release.json': 'release.json', base + 'release.json.sig': 'release.json.sig', base + self.filename: self.filename}
        assets = [{'filename': 'xdnote-' + version + '-' + suffix, 'platform': system, 'architecture': arch, 'package_type': kind, 'minimum_system': minimum, 'bytes': package.stat().st_size, 'sha256': digest(package), 'url': base + 'xdnote-' + version + '-' + suffix} for system, arch, kind, suffix, minimum in TARGETS]
        dump(self.run / 'release.json', {'schema_version': 1, 'product': 'xdnote', 'version': version, 'channel': 'alpha', 'build': 201, 'tag': 'v' + version, 'commit': identity['revision'], 'repository': 'xiaodaozhi/xdnote', 'assets': assets, 'macos_appcast_url': base + 'appcast.xml'})
        checked([self.signer, 'sign', self.seed, self.run / 'release.json', self.run / 'release.json.sig'])
        dump(self.run / 'listing.json', [{'tag_name': 'v' + version, 'draft': False, 'assets': [{'name': self.filename, 'size': package.stat().st_size, 'browser_download_url': base + self.filename}] + [{'name': name, 'browser_download_url': base + name} for name in ['release.json', 'release.json.sig']]}])

    def install_old(self):
        if self.windows:
            self.root = self.source / 'build/installed'
            command([self.packages['old'], '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/DIR=' + str(self.root)], log=self.work / 'install-old.log')
            self.image = self.root / 'xdnote.exe'
        else:
            command(['sudo', '-n', 'apt-get', '-o', 'Dir::Etc::sourcelist=' + str(ROOT / 'build/ci-apt/ubuntu.list'), '-o', 'Dir::Etc::sourceparts=-', '-y', '--no-remove', '--no-install-recommends', 'install', self.packages['old']], log=self.work / 'install-old.log')
            self.image = Path(Path('/usr/bin/xdnote').read_text().split('"')[1]); self.root = self.image.parent.parent

    def uninstall(self):
        if self.windows:
            uninstaller = self.root / 'unins000.exe'
            command([uninstaller, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'], log=self.work / 'uninstall.log')
            deadline = time.monotonic() + 30
            while uninstaller.exists() and time.monotonic() < deadline: time.sleep(.1)
            if uninstaller.exists(): raise RuntimeError('Owned uninstaller cleanup incomplete')
        else: command(['sudo', '-n', 'apt-get', '-y', 'purge', 'xdnote'], log=self.work / 'uninstall.log')

    def scenario(self, mode):
        self.install_old(); before = digest(self.image)
        for name in ['helper.json', 'error.json', 'status.json', 'restarted.json', 'cancelled.json', 'corrupt.json', 'recovery.json']: (self.run / name).unlink(missing_ok=True)
        (self.run / 'owned.md').write_bytes(b'owned\n'); dump(self.run / 'settings.json', {'schemaVersion': 1, 'fixtureUnknown': 'retained'})
        unrelated = {'id': '11111111-1111-4111-8111-111111111111', 'path': str(self.run / 'unrelated.md'), 'root': '', 'content': base64.b64encode(b'owned unrelated recovery\n').decode()}
        dump(self.run / 'settings.json.recovery', {'schemaVersion': 1, 'documents': [], 'drafts': [unrelated] if mode == 'discard' else []})
        dump(self.run / 'allowed.json', {'owner': 'xdnote-private-native-product-v1', 'root': str(self.root), 'executableSha256': before})
        dump(self.run / 'configuration.json', {'mode': mode, 'routes': self.routes})
        attempt = self.run / 'attempt.json'
        if mode == 'recovery': dump(attempt, {'schema_version': 1, 'transaction': 'a' * 32, 'phase': 'committed', 'version': '0.1.0-alpha.2', 'build': 201, 'commit': self.identities['new']['revision'], 'root': str(self.root), 'executable': str(self.image)})
        else: attempt.unlink(missing_ok=True)
        target = self.run / self.filename; shutil.copy2(self.packages['new'], target)
        if mode == 'corrupt':
            with target.open('r+b') as stream:
                stream.seek(target.stat().st_size // 2); value = stream.read(1); stream.seek(-1, 1); stream.write(bytes([value[0] ^ 1]))
        env = self.env.copy(); env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_CACHE_HOME=str(self.run / 'cache'), XDG_DATA_HOME=str(self.run / 'data'))
        if self.windows: env.update(APPDATA=str(self.run / 'appdata'), LOCALAPPDATA=str(self.run / 'localappdata'))
        args = [sys.executable, self.source / 'scripts/desktop.py', 'run', '--profile', 'default', '--build-dir', self.source / 'build/desktop', '--qt-prefix', self.qt, '--fixture-installation', self.root]
        checked(args, env=env, log=self.work / (mode + '.log'))
        report = self.run / ({'cancel': 'cancelled', 'install': 'restarted', 'discard': 'restarted'}.get(mode, mode) + '.json')
        result = json.loads(wait_file(report, timeout=180).read_bytes())
        if not result.get('ok'): raise RuntimeError('Product fixture failed: ' + mode)
        if mode not in ('install', 'discard') and digest(self.image) != before: raise RuntimeError('Old product unexpectedly changed')
        helper_file = self.run / 'helper.json'
        if helper_file.exists():
            helper = json.loads(helper_file.read_bytes()); receipt = Path(helper['directory']) / 'result.json'
            outcome = json.loads(wait_file(receipt, timeout=120).read_bytes())
            if outcome['transaction'] != helper['transaction'] or outcome['error'] != ('cancelled' if mode == 'cancel' else ''): raise RuntimeError('Native helper outcome differs')
            result['helperReceipt'] = outcome
            # ready/cancel/result belong to a known helper job. Wait for its
            # executable lease to retire before cleanup; never kill it.
            if self.windows:
                import ctypes
                kernel = ctypes.WinDLL('kernel32', use_last_error=True); kernel.OpenProcess.restype = ctypes.c_void_p
                handle = kernel.OpenProcess(0x00100000, False, int(helper['pid']))
                if handle:
                    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]; kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                    try:
                        if kernel.WaitForSingleObject(handle, 30000) != 0: raise TimeoutError('Owned native helper still active')
                    finally: kernel.CloseHandle(handle)
            else:
                import select
                try: fd = os.pidfd_open(int(helper['pid']))
                except ProcessLookupError: fd = None
                if fd is not None:
                    try:
                        poll = select.poll(); poll.register(fd, select.POLLIN)
                        if not poll.poll(30000): raise TimeoutError('Owned authorizer/helper still active')
                    finally: os.close(fd)
            result['helperExitObserved'] = True
        self.uninstall(); return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['work-dir', 'qt-prefix']: p.add_argument('--' + name, type=Path, required=True)
    for name in ['toolchain', 'runtime', 'vcvars']: p.add_argument('--' + name, type=Path)
    a = p.parse_args(); guard(); work = build_output(a.work_dir); qt = desktop.checked_tool_path(a.qt_prefix, 'Qt SDK', sdk=True)
    if work.exists(): p.error('Preserve existing evidence')
    work.mkdir(); trust = digest(ROOT / 'tools/update-trust.json'); opt_mode = None
    result = {'ok': False, 'scenarios': {}, 'releaseReady': False, 'scope': 'real private product versions, document save/cancel, native package installation and receipt/restart; injected file replies and temporary trust; no production network, minimum OS, visible wizard or polkit prompt acceptance'}
    try:
        if platform.system() == 'Linux':
            metadata = Path('/opt').stat()
            if metadata.st_uid != 0 or Path('/opt').is_symlink(): raise ValueError('Unexpected CI /opt')
            opt_mode = stat.S_IMODE(metadata.st_mode); command(['sudo', '-n', 'chmod', '0755', '/opt'])
        fixture = Product(work, qt, build_output(a.toolchain) if a.toolchain else None, build_output(a.runtime) if a.runtime else None, a.vcvars)
        result.update(sourceRevision=fixture.revision, fixtureIdentities=fixture.identities, privateSeedRemoved=not fixture.seed.exists())
        for mode in ['recovery', 'corrupt', 'cancel', 'install', 'discard']:
            result['scenarios'][mode] = fixture.scenario(mode); dump(work / 'report.json', result); print(mode + ' passed', flush=True)
        if digest(ROOT / 'tools/update-trust.json') != trust: raise ValueError('Production trust changed')
        result.update(ok=True, productionTrustUnchanged=True)
    finally:
        (work / 'source/build/native-run/private-seed').unlink(missing_ok=True)
        if opt_mode is not None: command(['sudo', '-n', 'chmod', format(opt_mode, 'o'), '/opt'])
        dump(work / 'report.json', result)
