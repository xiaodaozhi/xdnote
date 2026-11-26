import json
import os
from pathlib import Path
import shutil

import desktop
import sparkle
from check_native_update import TEMPLATES, checked, dump, replace
from installer_common import ROOT

def source_tree(work,qt,env):
    if checked(['git','-C',ROOT,'status','--porcelain']).stdout.strip():raise ValueError('Commit current fixture inputs before native verification')
    revision=checked(['git','-C',ROOT,'rev-parse','HEAD']).stdout.decode().strip()
    source=work/'source';checked(['git','-C',ROOT,'worktree','add','--detach',source,revision],log=work/'worktree.log')
    prepared=source/'build';prepared.mkdir()
    shutil.copytree(ROOT/'build/scintilla-5.6.7',prepared/'scintilla-5.6.7')
    sdk=sparkle.BASE/'arm64';sparkle.verify(sdk,built=True)
    shutil.copytree(sdk,prepared/'Sparkle'/sparkle.configuration()['framework_build']['variant']/'arm64',symlinks=True)
    # Same-project worktree ownership is checked before sharing tools and SDKs.
    if desktop.repository_owner(source)[1]!=desktop.repository_owner(ROOT)[1]:raise ValueError('Fixture worktree belongs to another project')
    replace(source/'scripts/desktop.py','str(BUILD / "cargo-home")',repr(str(ROOT/'build/cargo-home')))
    replace(source/'CMakeLists.txt','${PROJECT_SOURCE_DIR}/build/cargo-home',str(ROOT/'build/cargo-home'))
    fixture=source/'experiments/native-update-fixture';run=work/'run';run.mkdir()
    (fixture/'Fixture.mm').write_text((TEMPLATES/'Fixture.mm.in').read_text().replace('@DIRECTORY@',json.dumps(str(run),ensure_ascii=False)),encoding='utf-8')
    replace(source/'CMakeLists.txt','    target_sources(xdnote_desktop PRIVATE apps/desktop/src/MacUpdateProvider.mm)',
        '    target_sources(xdnote_desktop PRIVATE apps/desktop/src/MacUpdateProvider.mm experiments/native-update-fixture/Fixture.mm)\n    target_include_directories(xdnote_desktop PUBLIC experiments/native-update-fixture)\n    set_source_files_properties(experiments/native-update-fixture/Fixture.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")')
    replace(source/'apps/desktop/main.cpp','#include "BuildInfo.h"','#include "BuildInfo.h"\n#include "Fixture.h"')
    replace(source/'apps/desktop/main.cpp','    if (!installation.allowed()) {','    fixtureBoot(&application); return application.exec();\n    if (!installation.allowed()) {')
    controller=source/'apps/desktop/src/UpdateController.cpp'
    replace(controller,'#include "UpdateDownload.h"','#include "UpdateDownload.h"\n#include "Fixture.h"')
    replace(controller,'m_attemptPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/updates/attempt.json";','m_attemptPath = fixtureRoot() + "/../../attempt.json";')
    provider=source/'apps/desktop/src/MacUpdateProvider.mm'
    replace(provider,'#include "MacUpdateProvider.h"','#include "MacUpdateProvider.h"\n#include "Fixture.h"')
    text=provider.read_text();text=text.replace('    (void)error;\n    cancelDownload = nil;', '    fixtureNativeError(text(error.domain), error.code, text(error.localizedDescription), text(error.localizedFailureReason));\n    cancelDownload = nil;');provider.write_text(text)
    launcher=source/'scripts/desktop.py'
    replace(launcher,'    parser.add_argument("--fresh",','    parser.add_argument("--fixture-bundle", type=Path, help="private native update diagnostic only")\n    parser.add_argument("--fresh",')
    replace(launcher,'        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)', '''        if args.fixture_bundle:
            bundle = args.fixture_bundle.absolute()
            if bundle != Path.home() / 'Applications/xdnote.app' or bundle.is_symlink():
                raise ValueError('Only the owned public update fixture bundle may run')
            marker = json.loads((bundle.parent / '.fixture-allowed.json').read_text())
            executable = bundle / 'Contents/MacOS/xdnote'
            if marker.get('owner') != 'xdnote-private-native-update-v1' or hashlib.sha256(executable.read_bytes()).hexdigest() not in marker.get('allowedExecutableSha256', []):
                raise ValueError('Private fixture identity differs')
            env['QT_PLUGIN_PATH'] = str(bundle / 'Contents/PlugIns')
            env['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(bundle / 'Contents/PlugIns/platforms')
            env['QT_QPA_PLATFORM'] = 'cocoa'
        run([executable, *(path.absolute() for path in args.file)], env, stop_at_process_exit=True)''')
    settings = {'schemaVersion': 1, 'fixtureUnknown': 'retained',
                'updates': {'proxyMode': 'none', 'proxyAddress': '', 'mirror': ''}}
    dump(run / 'settings.json', settings)
    dump(run / 'configuration.json', {'mode': 'install'})
    (run / 'owned.md').write_text('owned\n')
    checked(['git', '-C', source, 'add', 'CMakeLists.txt', 'apps/desktop',
             'experiments/native-update-fixture', 'scripts/desktop.py'])
    checked(['git', '-C', source, '-c', 'user.name=xdnote CI Fixture',
             '-c', 'user.email=fixture@invalid.example', '-c', 'commit.gpgsign=false',
             'commit', '-m', 'test: drive the unmodified public macOS update transport'])
    predecessor = checked(['git','-C',source,'rev-parse','HEAD']).stdout.decode().strip()
    record = prepared / 'old-identity.json'
    dump(record, {'schemaVersion': 1, 'product': 'xdnote', 'version': '0.1.0-rc.2',
                  'build': 304, 'revision': predecessor, 'previousVersion': '0.1.0-rc.1',
                  'previousBuild': 2})
    checked([os.sys.executable, launcher, 'build', '--profile', 'default', '--build-dir',
             prepared / 'desktop', '--qt-prefix', qt, '--release-identity', record, '--parallel', '4'],
            env=env, log=work / 'old-build.log')
    checked([os.sys.executable, source / 'scripts/product_deployment_unix.py', '--build-dir',
             prepared / 'desktop', '--qt-prefix', qt, '--destination', prepared / 'old-product'],
            env=env, log=work / 'old-deployment.log')
    return source, run, revision
