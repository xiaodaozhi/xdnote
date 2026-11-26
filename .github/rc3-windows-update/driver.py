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
import signal
import stat
import subprocess
import sys
import time
import desktop
from native_fixture_proxy import Proxy, Origin, HOST
import threading
from check_native_update import replace, dump, checked
from check_update_helper import guard, command, wait_file
from installer_common import ROOT, build_output, digest
from release_candidate import TARGETS
from release_updates import KEY_ID, MANIFEST_KEY, SIGNING_VARIABLES, bounded, run_tool, build_tool, verify_release

TEMPLATES = ROOT / 'experiments/native-update-fixture'
REGRESSION_VERSIONS = ('0.0.0', '0.1.0-alpha.1', '0.1.0-alpha.2')
PRODUCTION_VERSIONS = ('0.0.0-alpha.199', '0.0.0-alpha.200', '0.0.0-alpha.201')


class Product:
    def __init__(self, work, qt, toolchain, runtime, vcvars, production_phase=None, sealed=None, attachments=None):
        self.work, self.qt, self.windows = work, qt, platform.system() == 'Windows'
        self.external = sealed is not None
        self.production = production_phase is not None or self.external
        self.versions = PRODUCTION_VERSIONS if self.production else REGRESSION_VERSIONS
        self.env = desktop.environment(vcvars); self.env['CARGO_NET_OFFLINE'] = 'true'
        self.source = work / 'source'; self.run = self.source / 'build/native-run'
        if command(['git', '-C', ROOT, 'status', '--porcelain']).stdout.strip(): raise ValueError('Commit fixture inputs first')
        self.revision = command(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.decode().strip()
        if self.external:
            self.external_identity = json.loads((sealed / 'identity.json').read_bytes())
            if (self.external_identity['revision'] != self.revision
                    or self.external_identity['build'] != 305
                    or self.external_identity['version'] != '0.1.0-rc.3'):
                raise ValueError('The sealed candidate does not match the frozen source and build')
            verify_release(self.external_identity, sealed / 'assets',
                           json.loads((sealed / 'verification.json').read_bytes()), attachments, build_tool())
            self.versions = ('0.0.0-alpha.301', '0.0.0-alpha.302', '0.1.0-rc.3')
        if self.production and not self.external and self.revision != os.environ.get('GITHUB_SHA'):
            raise ValueError('Production fixture must use the exact workflow revision')
        if production_phase in ('sign', 'run'):
            self.restore_production()
            return
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
        replace(controller, '    m_download = new UpdateDownload(m_network, this);', '    m_network->setParent(this);\n    m_download = new UpdateDownload(m_network, this);')
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
        if not self.production:
            self.prepare_fixture_signer()
        paths = ['CMakeLists.txt', 'apps/desktop', 'crates/update-helper', 'crates/update',
                 'experiments/native-update-fixture', 'scripts/desktop.py', 'tools/update-trust.json']
        checked(['git', '-C', self.source, 'add', *paths])
        checked(['git', '-C', self.source, '-c', 'user.name=xdnote CI Fixture', '-c', 'user.email=fixture@invalid.example', '-c', 'commit.gpgsign=false', 'commit', '-m', 'test: isolate real product update fixture'])
        previous, old, new = self.versions
        self.identities, self.packages = {}, {}
        builds = ([('old', old, 302, previous, 301)] if self.external else
                  [('old', old, 200, previous, 0), ('new', new, 201, old, 200)])
        for label, version, build, baseline, previous_build in builds:
            if label == 'new': checked(['git', '-C', self.source, '-c', 'user.name=xdnote CI Fixture', '-c', 'user.email=fixture@invalid.example', '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', 'test: identify private product target 201'])
            revision = checked(['git', '-C', self.source, 'rev-parse', 'HEAD']).stdout.decode().strip()
            identity = self.source / ('build/' + label + '-identity.json')
            dump(identity, {'schemaVersion': 1, 'product': 'xdnote', 'version': version, 'build': build, 'revision': revision, 'previousVersion': baseline, 'previousBuild': previous_build})
            args = [sys.executable, self.source / 'scripts/desktop.py', 'build', '--profile', 'default', '--build-dir', self.source / 'build/desktop', '--qt-prefix', qt, '--release-identity', identity, '--parallel', '4']
            if vcvars: args += ['--vcvars', vcvars]
            checked(args, env=self.env, log=work / (label + '-build.log'))
            self.identities[label] = json.loads((self.source / 'build/desktop/generated/build-version.json').read_bytes())
            self.packages[label] = self.package(label)
        if self.external:
            self.bind_sealed_candidate(sealed, attachments)
        else:
            self.candidate()
        if not self.production:
            checked([self.signer, 'sign', self.seed, self.run / 'release.json', self.run / 'release.json.sig'])
            self.seed.unlink()

    def bind_sealed_candidate(self, sealed, attachments):
        manifest = json.loads((attachments / 'release.json').read_bytes())
        self.identities['new'] = self.external_identity
        target = next(asset for asset in manifest['assets']
                      if asset['package_type'] == ('exe' if self.windows else 'deb'))
        self.filename = target['filename']
        self.packages['new'] = sealed / 'assets' / self.filename
        base = target['url'].removesuffix(self.filename)
        self.routes = {'https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=1': 'listing.json',
                       base + 'release.json': 'release.json', base + 'release.json.sig': 'release.json.sig',
                       target['url']: self.filename}
        for name in ('release.json', 'release.json.sig'):
            shutil.copyfile(attachments / name, self.run / name)
        listing = [{'name': item['filename'], 'size': item['bytes'], 'browser_download_url': item['url']}
                   for item in manifest['assets']]
        listing.extend({'name': name, 'size': (attachments / name).stat().st_size, 'browser_download_url': base + name}
                       for name in ('release.json', 'release.json.sig'))
        dump(self.run / 'listing.json', [{'tag_name': manifest['tag'], 'draft': False, 'assets': listing}])

    def running_target(self, executable):
        if self.windows:
            snippet = '$path = $env:XDNOTE_OWNED_RESTART\nGet-CimInstance Win32_Process -Filter "Name=\'xdnote.exe\'" |\n    Where-Object { $_.ExecutablePath -eq $path } |\n    Select-Object -ExpandProperty ProcessId'
            environment = dict(os.environ, XDNOTE_OWNED_RESTART=str(executable))
            output = command(['pwsh', '-NoProfile', '-NonInteractive', '-Command', snippet], env=environment).stdout
            return [int(pid) for pid in output.split()]
        result = []
        for child in Path('/proc').iterdir():
            if child.name.isdecimal():
                try:
                    if (child / 'exe').readlink() == executable:
                        result.append(int(child.name))
                except OSError:
                    pass
        return result

    def inspect_restart(self):
        executable = self.image if self.windows else Path(Path('/usr/bin/xdnote').read_text().split('"')[1])
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            pids = self.running_target(executable)
            if pids:
                break
            time.sleep(.25)
        else:
            raise RuntimeError('Native helper did not relaunch the installed product')
        if len(pids) != 1:
            raise RuntimeError('Unexpected installed processes')
        try:
            identity = json.loads(command([executable, '--version-json']).stdout)
            expected = {key: value for key, value in self.external_identity.items()
                        if key not in {'schemaVersion', 'configuration'}}
            document_ok = (self.run / 'owned.md').read_bytes() == b'edited owned\n'
            settings_ok = json.loads((self.run / 'settings.json').read_bytes())['fixtureUnknown'] == 'retained'
            if identity != expected or not document_ok or not settings_ok:
                raise RuntimeError('Restarted identity, saved document or settings differed')
            return {'ok': True, 'restartObserved': True, 'identity': identity,
                    'documentPreserved': document_ok, 'unknownSettingPreserved': settings_ok}
        finally:
            if pids[0] in self.running_target(executable):
                os.kill(pids[0], signal.SIGTERM)
            deadline = time.monotonic() + 15
            while pids[0] in self.running_target(executable) and time.monotonic() < deadline:
                time.sleep(.2)
            if pids[0] in self.running_target(executable):
                raise RuntimeError('Owned restarted application did not exit')

    def prepare_fixture_signer(self):
        examples = self.source / 'crates/update/examples'; examples.mkdir(exist_ok=True)
        shutil.copy2(TEMPLATES / 'fixture_sign.rs.in', examples / 'fixture_sign.rs')
        self.seed = self.run / 'private-seed'; self.seed.write_bytes(os.urandom(32)); self.seed.chmod(0o600)
        signer_target = self.work / 'signer'
        checked(['rustup', 'run', '1.99.0', 'cargo', 'build', '--locked', '--offline', '--manifest-path', self.source / 'Cargo.toml', '-p', 'xdnote-update', '--example', 'fixture_sign', '--target-dir', signer_target], env=self.env, log=self.work / 'signer.log')
        self.signer = signer_target / ('debug/examples/fixture_sign.exe' if self.windows else 'debug/examples/fixture_sign')
        public = checked([self.signer, 'public', self.seed]).stdout.decode().strip()
        dump(self.source / 'tools/update-trust.json', {'schema_version': 1, 'keys': [{'key_id': 'isolated-native-fixture', 'public_key': public}]})

    def restore_production(self):
        def git(*args):
            return command(['git', '-C', self.source, *args]).stdout.decode().strip()

        if git('rev-parse', 'HEAD^^') != self.revision or git('status', '--porcelain'):
            raise ValueError('The prepared fixture is not bound to this workflow revision')
        if (git('rev-parse', 'HEAD^{tree}') != git('rev-parse', 'HEAD^^{tree}') or
                digest(self.source / 'tools/update-trust.json') != digest(ROOT / 'tools/update-trust.json')):
            raise ValueError('Prepared source does not retain the production trust root')
        self.identities, self.packages = {}, {}
        for label, revision, version, build in [('old', git('rev-parse', 'HEAD^'), self.versions[1], 200),
                                               ('new', git('rev-parse', 'HEAD'), self.versions[2], 201)]:
            base = build_output(self.source / ('build/' + label))
            package = build_output(base.with_suffix('.deb'))
            report = json.loads(bounded(base.with_suffix('.json'), 256 * 1024))
            identity = {'schemaVersion': 1, 'product': 'xdnote', 'version': version, 'channel': 'alpha',
                        'build': build, 'revision': revision, 'dirty': False, 'configuration': 'Release'}
            if (report['ok'] is not True or report['buildIdentity']['buildVersion'] != identity or
                    report['file'] != package.name or not package.is_file() or package.stat().st_nlink != 1 or
                    not 0 < package.stat().st_size <= 2 * 1024**3 or
                    report['sha256'] != digest(package)):
                raise ValueError('The prepared Linux package differs from its build report')
            expected_version = '1:' + version.replace('-', '~', 1) + '+git' + revision
            package_fields = command(['dpkg-deb', '-f', package, 'Package', 'Architecture', 'Version']).stdout.decode()
            if set(package_fields.splitlines()) != {'Package: xdnote', 'Architecture: amd64',
                                                    'Version: ' + expected_version}:
                raise ValueError('The prepared Linux package identity differs')
            self.identities[label], self.packages[label] = identity, package
        manifest = self.candidate_manifest()
        expected = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()
        if bounded(self.run / 'release.json', 256 * 1024) != expected:
            raise ValueError('The private manifest differs from the prepared fixture')

    def patch_launcher(self):
        launcher = self.source / 'scripts/desktop.py'
        replace(launcher, '    parser.add_argument("--fresh",', '    parser.add_argument("--fixture-installation", type=Path)\n    parser.add_argument("--fresh",')
        replace(launcher, '        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)', '''        if args.fixture_installation:
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
        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)''')

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

    def candidate_manifest(self):
        identity = self.identities['new']; version = identity['version']; package = self.packages['new']
        suffix = 'windows-x86_64-setup.exe' if self.windows else 'ubuntu22.04-x86_64.deb'
        self.filename = 'xdnote-' + version + '-' + suffix
        base = 'https://github.com/xiaodaozhi/xdnote/releases/download/v' + version + '/'
        self.routes = {'https://api.github.com/repos/xiaodaozhi/xdnote/releases?per_page=100&page=1': 'listing.json', base + 'release.json': 'release.json', base + 'release.json.sig': 'release.json.sig', base + self.filename: self.filename}
        size, sha256 = package.stat().st_size, digest(package)
        assets = []
        for system, arch, kind, target, minimum in TARGETS:
            name = 'xdnote-' + version + '-' + target
            # The public schema requires all roles. Only the private .deb is served.
            length, sha = (1, '0' * 64) if self.production and name != self.filename else (size, sha256)
            assets.append({'filename': name, 'platform': system, 'architecture': arch, 'package_type': kind,
                           'minimum_system': minimum, 'bytes': length, 'sha256': sha, 'url': base + name})
        manifest = {'schema_version': 1, 'product': 'xdnote', 'version': version, 'channel': 'alpha', 'build': 201,
                    'tag': 'v' + version, 'commit': identity['revision'], 'repository': 'xiaodaozhi/xdnote', 'assets': assets}
        if not self.production:
            manifest['macos_appcast_url'] = base + 'appcast.xml'
        return manifest

    def candidate(self):
        manifest = self.candidate_manifest()
        dump(self.run / 'release.json', manifest)
        offered = next(asset for asset in manifest['assets'] if asset['filename'] == self.filename)
        base = offered['url'].removesuffix(self.filename)
        dump(self.run / 'listing.json', [{'tag_name': manifest['tag'], 'draft': False,
             'assets': [{'name': self.filename, 'size': offered['bytes'], 'browser_download_url': offered['url']}] +
                       [{'name': name, 'browser_download_url': base + name} for name in ['release.json', 'release.json.sig']]}])

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

    def scenario(self, mode, transport="none", origin=None, proxy=None):
        self.install_old(); before = digest(self.image)
        for name in ['helper.json', 'error.json', 'status.json', 'restarted.json', 'cancelled.json', 'corrupt.json', 'recovery.json']: (self.run / name).unlink(missing_ok=True)
        (self.run / 'owned.md').write_bytes(b'owned\n'); dump(self.run / 'settings.json', {'schemaVersion': 1, 'fixtureUnknown': 'retained', 'updates': {'proxyMode': 'custom' if proxy else transport, 'proxyAddress': f'{transport}://127.0.0.1:{proxy.server_address[1]}' if proxy else '', 'mirror': ''}})
        unrelated = {'id': '11111111-1111-4111-8111-111111111111', 'path': str(self.run / 'unrelated.md'), 'root': '', 'content': base64.b64encode(b'owned unrelated recovery\n').decode()}
        dump(self.run / 'settings.json.recovery', {'schemaVersion': 1, 'documents': [], 'drafts': [unrelated] if mode == 'discard' else []})
        dump(self.run / 'allowed.json', {'owner': 'xdnote-private-native-product-v1', 'root': str(self.root), 'executableSha256': before})
        dump(self.run / 'configuration.json', {'mode': mode, 'routes': self.routes, 'package': self.filename, 'server': f'http://{HOST}:{origin.server_address[1]}/' if origin else ''})
        if origin: origin.disconnect_archive = mode == 'network'
        start = len(origin.requests) if origin else 0; proxy_start = len(proxy.records) if proxy else 0
        attempt = self.run / 'attempt.json'
        if mode == 'recovery': dump(attempt, {'schema_version': 1, 'transaction': 'a' * 32, 'phase': 'committed', 'version': self.identities['new']['version'], 'build': 201, 'commit': self.identities['new']['revision'], 'root': str(self.root), 'executable': str(self.image)})
        else: attempt.unlink(missing_ok=True)
        target = self.run / self.filename; shutil.copy2(self.packages['new'], target)
        if mode == 'corrupt':
            with target.open('r+b') as stream:
                stream.seek(target.stat().st_size // 2); value = stream.read(1); stream.seek(-1, 1); stream.write(bytes([value[0] ^ 1]))
        env = self.env.copy(); env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_CACHE_HOME=str(self.run / 'cache'), XDG_DATA_HOME=str(self.run / 'data'))
        if self.windows: env.update(APPDATA=str(self.run / 'appdata'), LOCALAPPDATA=str(self.run / 'localappdata'))
        args = [sys.executable, self.source / 'scripts/desktop.py', 'run', '--profile', 'default', '--build-dir', self.source / 'build/desktop', '--qt-prefix', self.qt, '--fixture-installation', self.root]
        try: checked(args, env=env, log=self.work / (transport + '-' + mode + '.log'))
        finally: dump(self.work / (transport + '-' + mode + '-transport.json'), {'originRequests': origin.requests[start:] if origin else [], 'proxyConnections': proxy.records[proxy_start:] if proxy else [], 'proxyErrors': proxy.errors if proxy else []})
        report = self.run / ({'cancel': 'cancelled', 'install': 'restarted', 'discard': 'restarted', 'network': 'corrupt'}.get(mode, mode) + '.json')
        external_install = self.external and mode == 'install'
        result = {'ok': True} if external_install else json.loads(wait_file(report, timeout=180).read_bytes())
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
        if proxy:
            records = proxy.records[proxy_start:]; requests = origin.requests[start:]; ports = {r['originClientPort'] for r in records if r['accepted']}
            if not ports or not requests or any(r['clientPort'] not in ports for r in requests): raise RuntimeError('Package transfer escaped observed private proxy')
            result['transport'] = {'mode': transport, 'proxyConnections': records, 'originRequests': requests}
        if external_install:
            if 'helperReceipt' not in result:
                raise RuntimeError('Native transaction receipt missing')
            result.update(self.inspect_restart())
        self.uninstall()
        return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['work-dir', 'qt-prefix']: p.add_argument('--' + name, type=Path, required=True)
    for name in ['toolchain', 'runtime', 'vcvars']: p.add_argument('--' + name, type=Path)
    p.add_argument('--proxy-matrix', action='store_true')
    p.add_argument('--sealed-candidate', type=Path)
    p.add_argument('--signed-attachments', type=Path)
    p.add_argument('--production-phase', choices=('prepare', 'sign', 'run'))
    credentials = {name: os.environ.pop(name) for name in SIGNING_VARIABLES if name in os.environ}
    a = p.parse_args(); guard(); work = build_output(a.work_dir); qt = desktop.checked_tool_path(a.qt_prefix, 'Qt SDK', sdk=True)
    phase = a.production_phase
    external = a.sealed_candidate is not None
    if external and (a.signed_attachments is None or phase or a.proxy_matrix or
                     os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted' or
                     os.environ.get('GITHUB_REPOSITORY') != 'xiaodaozhi/xdnote' or
                     os.environ.get('GITHUB_REF_NAME') != 'coding/155-rc3-windows-update'):
        p.error('The exact private candidate is accepted only on the authorized disposable runners')
    if phase and (work != ROOT / 'build/production-linux-update' or os.environ.get('GITHUB_REF') != 'refs/heads/main' or
                  os.environ.get('GITHUB_REPOSITORY') != 'xiaodaozhi/xdnote' or
                  os.environ.get('RUNNER_ENVIRONMENT') != 'github-hosted' or platform.system() != 'Linux' or
                  platform.machine() != 'x86_64' or platform.freedesktop_os_release().get('VERSION_ID') != '22.04' or
                  platform.freedesktop_os_release().get('ID') != 'ubuntu' or a.proxy_matrix):
        p.error('Use the dedicated hosted Ubuntu 22.04 main workflow')
    if (phase == 'sign' and set(credentials) != {KEY_ID, MANIFEST_KEY}) or (phase != 'sign' and credentials):
        p.error('Only the protected signature step may receive the manifest signing key')
    tool = ROOT / 'build/release-tool/release/xdnote-update-tool'
    if phase in ('sign', 'run'):
        fixture = Product(work, qt, None, None, None, phase)
        manifest, signature = (build_output(fixture.run / name) for name in ('release.json', 'release.json.sig'))
        operation = 'sign' if phase == 'sign' else 'verify'
        run_tool(build_output(tool), operation, manifest, signature, credentials=credentials)
        if phase == 'sign':
            return
    else:
        if work.exists(): p.error('Preserve existing evidence')
        work.mkdir()
        fixture = None
    trust = digest(ROOT / 'tools/update-trust.json'); opt_mode = None
    result = {'ok': False, 'scenarios': {}, 'releaseReady': False, 'scope': 'real private product versions, document save/cancel, native package installation and receipt/restart; owned metadata replies, optional controlled HTTP proxy package streams and temporary trust; no production TLS network, minimum OS, visible wizard or polkit prompt acceptance'}
    try:
        if platform.system() == 'Linux' and phase != 'prepare':
            metadata = Path('/opt').stat()
            if metadata.st_uid != 0 or Path('/opt').is_symlink(): raise ValueError('Unexpected CI /opt')
            opt_mode = stat.S_IMODE(metadata.st_mode); command(['sudo', '-n', 'chmod', '0755', '/opt'])
        if fixture is None:
            fixture = Product(work, qt, build_output(a.toolchain) if a.toolchain else None,
                              build_output(a.runtime) if a.runtime else None, a.vcvars, phase,
                              build_output(a.sealed_candidate) if external else None,
                              build_output(a.signed_attachments) if external else None)
        result.update(sourceRevision=fixture.revision, fixtureIdentities=fixture.identities,
                      privateSeedRemoved=not (fixture.run / 'private-seed').exists())
        if phase:
            result.update(scope='private Linux product transactions using the compiled production public key and local file replies',
                          productionSignatureVerified=phase == 'run', productionTrustSha256=trust,
                          packageSha256=digest(fixture.packages['new']), prepared=True)
        if phase == 'prepare':
            return
        if external:
            result.update(scope='production trust, signed six-asset candidate and native update of a private predecessor',
                          verifiedPackageSha256=digest(fixture.packages['new']))
        modes = ['install'] if external else ['recovery', 'corrupt', 'cancel', 'install', 'discard']
        for mode in modes:
            result['scenarios'][mode] = fixture.scenario(mode); dump(work / 'report.json', result); print(mode + ' passed', flush=True)
        if a.proxy_matrix:
            origin = Origin(fixture.run / fixture.filename); threading.Thread(target=origin.serve_forever, daemon=True).start()
            try:
                for transport in ['http', 'socks5']:
                    proxy = Proxy(transport, origin.server_address[1]); threading.Thread(target=proxy.serve_forever, daemon=True).start()
                    try:
                        for mode in ['network', 'cancel', 'install', 'discard']:
                            key = transport + '-' + mode
                            result['scenarios'][key] = fixture.scenario(mode, transport, origin, proxy); dump(work / 'report.json', result); print(key + ' passed', flush=True)
                    finally: proxy.shutdown(); proxy.server_close()
            finally: origin.shutdown(); origin.server_close()
        if digest(ROOT / 'tools/update-trust.json') != trust: raise ValueError('Production trust changed')
        result.update(ok=True, productionTrustUnchanged=True)
    finally:
        (work / 'source/build/native-run/private-seed').unlink(missing_ok=True)
        if opt_mode is not None: command(['sudo', '-n', 'chmod', format(opt_mode, 'o'), '/opt'])
        dump(work / 'report.json', result)


if __name__ == '__main__':
    main()
