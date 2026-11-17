"""Embed only pinned Sparkle runtime files and preserve helper signing policy."""
import base64
import json
import os
from pathlib import Path
import plistlib
import shutil
import sparkle
from installer_common import digest
from unix_binary_evidence import code_sections

INSTALL_ID = 'io.github.xiaodaozhi.xdnote.release.v1'


def sdk():
    source = sparkle.BASE / 'arm64'
    sparkle.verify(source, built=True)
    return source


def embed(app, checked, evidence, identity):
    source = sdk()
    framework = app / 'Contents/Frameworks/Sparkle.framework'
    if framework.is_symlink() or not framework.resolve().is_relative_to(app.resolve()):
        raise ValueError('Unbounded Sparkle deployment')
    if framework.exists(): shutil.rmtree(framework)
    shutil.copytree(source / 'Sparkle.framework', framework, symlinks=True)
    # Build headers/module maps are SDK inputs, not runtime product files.
    for name in ('Headers', 'PrivateHeaders', 'Modules'):
        directory = framework / 'Versions/B' / name
        if directory.exists(): shutil.rmtree(directory)
        alias = framework / name
        if alias.is_symlink(): alias.unlink()
    # The non-sandboxed product uses the in-process downloader and launcher.
    # These optional XPC services are neither enabled nor shipped.
    shutil.rmtree(framework / 'Versions/B/XPCServices')
    if (framework / 'XPCServices').is_symlink(): (framework / 'XPCServices').unlink()
    targets = [framework / 'Versions/B/Autoupdate', framework / 'Versions/B/Updater.app']
    metadata = []
    for target in targets:
        text = checked(['/usr/bin/codesign', '-d', '--entitlements', ':-', '--xml', target]).strip()
        entitlements = plistlib.loads(text.encode()) if text else {}
        if entitlements.get('com.apple.security.get-task-allow'): raise ValueError('Release helper has debugger entitlement')
        metadata.append((target, entitlements))
    for path in sorted(framework.rglob('*')):
        if path.is_symlink():
            if os.path.isabs(os.readlink(path)) or not path.resolve(strict=True).is_relative_to(framework): raise ValueError('Unbounded Sparkle link')
            continue
        if not path.is_file(): continue
        if path.read_bytes()[:4] not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'): continue
        architectures = checked(['/usr/bin/lipo', '-archs', path]).split()
        if 'arm64' not in architectures: raise ValueError('Sparkle runtime lacks arm64')
        if architectures != ['arm64']:
            temporary = path.with_name(path.name + '.arm64-staged')
            checked(['/usr/bin/lipo', path, '-thin', 'arm64', '-output', temporary])
            temporary.chmod(path.stat().st_mode & 0o777); temporary.replace(path)
    for index, (target, entitlements) in enumerate(metadata):
        args = ['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', '--options', 'runtime']
        if entitlements:
            record = evidence / ('sparkle-helper-' + str(index) + '.entitlements')
            record.write_bytes(plistlib.dumps(entitlements)); args += ['--entitlements', record]
        checked(args + [target])
    checked(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', '--options', 'runtime', framework])
    checked(['/usr/bin/codesign', '--verify', '--deep', '--strict', framework])
    info_path = app / 'Contents/Info.plist'; info = plistlib.loads(info_path.read_bytes())
    info.update(CFBundleVersion=str(identity['build']), CFBundleShortVersionString=identity['version'],
                SUEnableAutomaticChecks=False, SUAutomaticallyUpdate=False, SUEnableSystemProfiling=False,
                SUEnableInstallerLauncherService=False, SUEnableDownloaderService=False,
                XdnoteUpdateInstallable=False, XdnoteUpdateInstallId=INSTALL_ID)
    key = json.loads((sparkle.ROOT / 'tools/sparkle-update-key.json').read_text(encoding='utf-8'))
    if set(key) != {'schemaVersion', 'publicKey'} or key['schemaVersion'] != 1: raise ValueError('Invalid Sparkle key record')
    public = key['publicKey']
    if public:
        if len(base64.b64decode(public, validate=True)) != 32: raise ValueError('Invalid Sparkle public key')
        info['SUPublicEDKey'] = public
        info['XdnoteUpdateInstallable'] = identity['channel'] != 'development' and not identity['dirty'] and identity['build'] > 0
    info_path.write_bytes(plistlib.dumps(info))
    notice = app / 'Contents/Resources/licenses/Sparkle'; notice.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'LICENSE', notice / 'LICENSE')
    shutil.copyfile(source / sparkle.RECEIPT, notice / 'sdk-receipt.json')
    (notice / 'provenance.json').write_text(json.dumps(sparkle.configuration(), indent=2) + '\n')
    return framework


def evidence(path, source):
    if not source.is_file(): raise ValueError('Missing Sparkle supplier file')
    record = {'kind': 'Sparkle runtime', 'sdkFile': source.relative_to(sdk()).as_posix(), 'sdkSha256': digest(source),
              'byteIdenticalToSdk': digest(source) == digest(path), 'license': sparkle.configuration()['license']}
    if path.read_bytes()[:4] == b'\xcf\xfa\xed\xfe':
        before = code_sections(source.read_bytes(), architecture='arm64'); after = code_sections(path.read_bytes())
        if before != after: raise ValueError('Deployed Sparkle instructions differ')
        record.update(nativeSections=after, nativeSectionsMatchSdk=True)
    elif not record['byteIdenticalToSdk'] and '_CodeSignature' not in path.parts:
        raise ValueError('Unexplained Sparkle resource change')
    return record
