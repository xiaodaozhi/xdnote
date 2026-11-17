"""Exercise real native helper transactions with tiny owned process images in CI.

This changes only a disposable Actions account. Linux authorizes the unchanged
root helper through sudo instead of a human polkit prompt. Windows adds silent
Setup switches only in a private source copy. No desktop or production release
acceptance is inferred from these synthetic images and ephemeral trust roots.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import secrets
import shutil
import subprocess
import time
from installer_common import ROOT, build_output, digest
from installer_payload import inventory
from check_native_update import replace, dump
from release_candidate import TARGETS


def command(args, *, log=None, env=None, check=True):
    result = subprocess.run(list(map(str, args)), capture_output=True, env=env, timeout=300)
    if log: log.write_bytes(result.stdout + result.stderr)
    if check and result.returncode: raise RuntimeError('Helper fixture command failed: ' + str(args[0]))
    return result


def wait_file(path, process=None, timeout=90):
    deadline = time.monotonic() + timeout
    while not path.is_file():
        if process and process.poll() is not None: raise RuntimeError('Fixture process exited before ' + path.name)
        if time.monotonic() > deadline: raise TimeoutError(path.name)
        time.sleep(.05)
    return path


class Fixture:
    def __init__(self, work, toolchain):
        self.work, self.toolchain = work, toolchain
        self.windows = platform.system() == 'Windows'
        self.suffix = '.exe' if self.windows else ''
        self.environment = os.environ.copy()
        self.environment['CARGO_HOME'] = str(ROOT / 'build/cargo-home')
        self.revision = command(['git', '-C', ROOT, 'rev-parse', 'HEAD']).stdout.decode().strip()
        self.source = work / 'source'
        self.source.mkdir()
        for name in ['Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml']: shutil.copy2(ROOT / name, self.source / name)
        shutil.copytree(ROOT / 'crates', self.source / 'crates')
        (self.source / 'tools').mkdir()
        examples = self.source / 'crates/update/examples'; examples.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'experiments/native-update-fixture/fixture_sign.rs.in', examples / 'fixture_sign.rs')
        self.seed = work / 'private-seed'; self.seed.write_bytes(secrets.token_bytes(32)); self.seed.chmod(0o600)
        self.target = work / 'target'
        self.cargo(['-p', 'xdnote-update', '--example', 'fixture_sign'], 'signer-build.log')
        self.signer = self.target / ('debug/examples/fixture_sign' + self.suffix)
        public = command([self.signer, 'public', self.seed]).stdout.decode().strip()
        dump(self.source / 'tools/update-trust.json', {'schema_version': 1, 'keys': [{'key_id': 'isolated-native-fixture', 'public_key': public}]})
        if self.windows:
            replace(self.source / 'crates/update-helper/src/windows.rs', '.arg("/NORESTART")', '.arg("/VERYSILENT").arg("/SUPPRESSMSGBOXES").arg("/NORESTART")')
        self.cargo(['-p', 'xdnote-update-helper'], 'helper-build.log')
        self.helper = self.target / ('debug/xdnote-update-helper' + self.suffix)
        self.restarted = work / 'restarted.json'
        self.identities = {}
        for label, version, build in [('old', '0.1.0-alpha.1', 200), ('new', '0.1.0-alpha.2', 201)]:
            identity = {'product': 'xdnote', 'version': version, 'build': build, 'revision': self.revision, 'dirty': False}
            self.identities[label] = identity
            text = (ROOT / 'experiments/native-update-fixture/helper_image.rs.in').read_text()
            text = text.replace('@IDENTITY@', json.dumps(json.dumps(identity))).replace('@RESTARTED@', json.dumps(str(self.restarted)))
            image_source = work / (label + '.rs'); image_source.write_text(text)
            command(['rustup', 'run', '1.99.0', 'rustc', '--edition=2021', image_source, '-o', work / (label + self.suffix)], log=work / (label + '-image.log'))
        self.packages = {label: self.package(label) for label in ['old', 'new', 'wrong']}
        self.manifests = {}
        for label in ['new', 'wrong']:
            package = self.packages[label]; identity = self.identities['new']; version = identity['version']
            base = 'https://github.com/xiaodaozhi/xdnote/releases/download/v' + version + '/'
            assets = [{'filename': 'xdnote-' + version + '-' + suffix, 'platform': system, 'architecture': arch,
                       'package_type': kind, 'minimum_system': minimum, 'bytes': package.stat().st_size,
                       'sha256': digest(package), 'url': base + 'xdnote-' + version + '-' + suffix}
                      for system, arch, kind, suffix, minimum in TARGETS]
            manifest = work / (label + '-release.json'); signature = work / (label + '-release.sig')
            dump(manifest, {'schema_version': 1, 'product': 'xdnote', 'version': version, 'channel': 'alpha', 'build': 201,
                            'tag': 'v' + version, 'commit': self.revision, 'repository': 'xiaodaozhi/xdnote', 'assets': assets,
                            'macos_appcast_url': base + 'appcast.xml'})
            command([self.signer, 'sign', self.seed, manifest, signature])
            self.manifests[label] = (manifest, signature)
        self.seed.unlink() # Signing authority is absent during every transaction.

    def cargo(self, args, log):
        command(['rustup', 'run', '1.99.0', 'cargo', 'build', '--locked', '--offline', '--manifest-path', self.source / 'Cargo.toml',
                 '--target-dir', self.target, *args], env=self.environment, log=self.work / log)

    def package(self, label):
        image = 'old' if label == 'wrong' else label
        identity = self.identities['new' if label == 'wrong' else label]
        payload = self.work / (label + '-payload'); payload.mkdir()
        if not self.windows:
            from build_linux_installer import package_tree
            (payload / 'bin').mkdir(); shutil.copy2(self.work / image, payload / 'bin/xdnote'); shutil.copy2(self.helper, payload / 'bin/xdnote-update-helper')
            audit = {'ok': True, 'host': {'system': 'Linux', 'architecture': 'x86_64'}, 'files': inventory(payload), 'imports': [], 'buildIdentity': {'buildVersion': identity}}
            # The standard package builder supplies hooks, marker, version directory and launcher.
            stage = self.work / (label + '-stage'); package_tree(payload, audit, stage)
            # This synthetic image has no Qt/UI dependencies. Keep the standard
            # package hooks/layout but declare only its actual runtime needs.
            control = stage / 'DEBIAN/control'; lines = control.read_text().splitlines()
            control.write_text('\n'.join('Depends: libc6 (>= 2.35), util-linux, desktop-file-utils, shared-mime-info' if line.startswith('Depends: ') else line for line in lines) + '\n')
            package = self.work / (label + '.deb')
            command(['/usr/bin/dpkg-deb', '--root-owner-group', '--build', stage, package], log=self.work / (label + '-package.log'))
        else:
            spec = importlib.util.spec_from_file_location('native_fixture_installer', ROOT / 'scripts/build-windows-installer.py')
            engine = importlib.util.module_from_spec(spec); spec.loader.exec_module(engine)
            shutil.copy2(self.work / (image + '.exe'), payload / 'xdnote.exe')
            notices = payload / 'share/notices/Microsoft-VC-runtime'; notices.mkdir(parents=True)
            (notices / 'license.rtf').write_text(r'{\rtf1 Standalone CI fixture. No Qt or VC runtime is shipped.}')
            (notices.parent / 'Windows-runtime-NOTICE.txt').write_text('Owned standalone helper transaction fixture.')
            audit = {'ok': True, 'files': inventory(payload)}
            package = self.work / (label + '.exe')
            text = engine.render(payload, audit, package, self.toolchain / 'ChineseSimplified.isl', '0.1.0-dev.' + self.revision[:12])
            package.with_suffix('.files').write_text(engine.payload_manifest(audit), encoding='utf-8')
            package.with_suffix('.iss').write_text(text, encoding='utf-8-sig')
            command([self.toolchain / 'compiler/ISCC.exe', package.with_suffix('.iss')], log=self.work / (label + '-package.log'))
        return package

    def install_old(self):
        if self.windows:
            self.root = self.work / 'installed'
            command([self.packages['old'], '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/DIR=' + str(self.root)], log=self.work / 'install-old.log')
            self.image = self.root / 'xdnote.exe'
        else:
            # A previous successful scenario is removed first, never downgrade an installed package.
            command(['sudo', '-n', 'apt-get', '-y', '--no-remove', '--no-install-recommends', 'install', self.packages['old']], log=self.work / 'install-old.log')
            launcher = Path('/usr/bin/xdnote').read_text()
            self.image = Path(launcher.split('"')[1]); self.root = self.image.parent.parent
        self.restarted.unlink(missing_ok=True)

    def uninstall(self):
        if self.windows:
            uninstaller = self.root / 'unins000.exe'
            if uninstaller.is_file(): command([uninstaller, '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'], log=self.work / 'uninstall.log')
        else:
            command(['sudo', '-n', 'apt-get', '-y', 'remove', 'xdnote'], log=self.work / 'uninstall.log')

    def request(self, label, parent, package_label='new'):
        job = self.work / ('job-' + label); job.mkdir(mode=0o700)
        if self.windows: command(['icacls.exe', job, '/inheritance:r', '/grant:r', '*S-1-3-4:(OI)(CI)F'], log=job / 'acl.log')
        transaction = secrets.token_hex(16)
        name = 'xdnote-0.1.0-alpha.2-' + ('windows-x86_64-setup.exe' if self.windows else 'ubuntu22.04-x86_64.deb')
        package = job / name; shutil.copy2(self.packages[package_label], package)
        manifest, signature = self.manifests[package_label]
        shutil.copy2(manifest, job / 'release.json'); shutil.copy2(signature, job / 'release.json.sig')
        dump(job / 'request.json', {'schema_version': 1, 'transaction': transaction, 'parent_pid': parent.pid,
             'current_version': '0.1.0-alpha.1', 'current_build': 200, 'install_root': str(self.root),
             'package_file': str(package), 'manifest_file': str(job / 'release.json'), 'signature_file': str(job / 'release.json.sig')})
        return job, transaction, package

    def helper_process(self, job):
        args = [self.helper, '--request', job / 'request.json']
        if not self.windows: args = ['sudo', '-n', 'env', 'PKEXEC_UID=' + str(os.getuid()), *args]
        stream = (job / 'helper.log').open('wb')
        process = subprocess.Popen(list(map(str, args)), stdout=stream, stderr=stream); stream.close()
        return process

    def scenario(self, mode):
        self.install_old()
        parent_directory = self.work / ('parent-' + mode); parent_directory.mkdir()
        parent = subprocess.Popen([str(self.image), '--wait', str(parent_directory)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_file(parent_directory / 'parent-ready', parent)
        job, transaction, package = self.request(mode, parent, 'wrong' if mode == 'identity_mismatch' else 'new')
        if mode == 'untrusted':
            envelope = json.loads((job / 'release.json.sig').read_bytes()); envelope['key_id'] = 'unknown-fixture-key'; dump(job / 'release.json.sig', envelope)
        if mode == 'corrupt':
            with package.open('r+b') as stream:
                stream.seek(package.stat().st_size // 2); value = stream.read(1); stream.seek(-1, 1); stream.write(bytes([value[0] ^ 1]))
        helper = self.helper_process(job)
        try:
            if mode in ('corrupt', 'untrusted'):
                helper.wait(timeout=60)
            else:
                wait_file(job / 'ready.json', helper)
                assert parent.poll() is None and not self.restarted.exists()
                if mode == 'concurrent':
                    other, _, _ = self.request(mode + '-second', parent)
                    second = self.helper_process(other); second.wait(timeout=60)
                    assert json.loads((other / 'result.json').read_bytes())['error'] == 'update_in_progress'
                if mode in ('cancel', 'concurrent'):
                    dump(job / 'cancel.json', {'transaction': transaction}); helper.wait(timeout=60)
                else:
                    dump(job / 'proceed.json', {'transaction': transaction})
                    time.sleep(1)
                    assert helper.poll() is None and parent.poll() is None and not self.restarted.exists()
                    (parent_directory / 'stop-parent').touch(); parent.wait(timeout=10)
                    helper.wait(timeout=120)
            result = json.loads((job / 'result.json').read_bytes())
            assert result['transaction'] == transaction
            expected = {'untrusted': 'untrusted_signature', 'corrupt': 'package_hash', 'cancel': 'cancelled',
                        'concurrent': 'cancelled', 'identity_mismatch': 'installed_identity_mismatch', 'install': ''}[mode]
            assert result['error'] == expected, result
            if mode == 'install':
                assert json.loads(wait_file(self.restarted).read_bytes()) == self.identities['new']
            else: assert not self.restarted.exists()
            return {'ok': True, 'result': result, 'parentExitBeforeInstallObserved': mode in ('install', 'identity_mismatch'),
                    'restartIdentity': self.identities['new'] if mode == 'install' else None}
        finally:
            (parent_directory / 'stop-parent').touch(); parent.wait(timeout=10)
            # Never kill a real installer/helper or uninstall while one still runs.
            if helper.poll() is not None: self.uninstall()


def guard():
    if os.environ.get('CI') != 'true' or os.environ.get('GITHUB_ACTIONS') != 'true' or platform.system() not in ('Linux', 'Windows'):
        raise RuntimeError('Disposable Windows/Linux GitHub Actions runners only')
    if platform.system() == 'Linux':
        if os.geteuid() == 0 or command(['dpkg-query', '-W', 'xdnote'], check=False).returncode == 0:
            raise RuntimeError('Require an unprivileged runner without an existing xdnote package')
        if Path('/usr/bin/xdnote').exists() or Path('/usr/libexec/xdnote-update-helper').exists(): raise RuntimeError('Foreign installation exists')
    else:
        import winreg
        try: winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\xdnote\Install').Close()
        except FileNotFoundError: pass
        else: raise RuntimeError('Existing user installation must be preserved')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--work-dir', type=Path, required=True); p.add_argument('--toolchain', type=Path)
    a = p.parse_args(); guard(); work = build_output(a.work_dir)
    if work.exists(): p.error('Choose a fresh fixture directory')
    if platform.system() == 'Windows' and not a.toolchain: p.error('Explicit prepared Inno Setup toolchain required')
    toolchain = build_output(a.toolchain) if a.toolchain else None
    work.mkdir(); trust = digest(ROOT / 'tools/update-trust.json')
    result = {'ok': False, 'scenarios': {}, 'releaseReady': False, 'scope': 'real native helper, installer and restart with synthetic standalone process images; no Qt desktop, polkit prompt, visible wizard or production trust acceptance'}
    try:
        fixture = Fixture(work, toolchain)
        for mode in ['untrusted', 'corrupt', 'cancel', 'concurrent', 'identity_mismatch', 'install']:
            result['scenarios'][mode] = fixture.scenario(mode); dump(work / 'report.json', result); print(mode + ' passed', flush=True)
        if digest(ROOT / 'tools/update-trust.json') != trust: raise ValueError('Production trust changed')
        result.update(ok=True, productionTrustUnchanged=True, privateSeedRemoved=not fixture.seed.exists(), sourceRevision=fixture.revision)
    finally: dump(work / 'report.json', result)
