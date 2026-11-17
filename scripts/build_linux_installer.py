"""Prepare a native Ubuntu 22.04 amd64 development package from a sealed runtime."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
from installer_common import ROOT, build_output, digest
from product_deployment_unix import inventory


def system_packages(path):
    """dpkg may retain /lib names on an usr-merged host; prove aliases first."""
    source = PurePosixPath(path)
    candidates = [source]
    if str(source).startswith('/usr/lib/'):
        alias = PurePosixPath('/lib') / source.relative_to('/usr/lib')
        if Path(alias).is_file() and Path(alias).samefile(Path(source)): candidates.append(alias)
    for candidate in candidates:
        query = subprocess.run(['/usr/bin/dpkg-query', '-S', str(candidate)], capture_output=True, text=True)
        if query.returncode != 0: continue
        names = set()
        for row in query.stdout.splitlines():
            name = row.split(': ')[0].removesuffix(':amd64')
            if not re.fullmatch('[a-z0-9][a-z0-9+.-]*', name): raise ValueError('Invalid system package owner')
            names.add(name)
        if names: return names
    raise ValueError('No dpkg owner for actual system library: ' + str(source))


def package_tree(payload, audit, stage):
    if not audit.get('ok') or audit.get('host') != {'system': 'Linux', 'architecture': 'x86_64'} or audit.get('files') != inventory(payload):
        raise ValueError('A sealed, audited Linux x86_64 runtime is required')
    identity = audit['buildIdentity']['buildVersion']; revision = identity['revision']
    if not re.fullmatch('[0-9a-f]{40}', revision): raise ValueError('Unknown source identity')
    version = '1:' + identity['version'].replace('-', '~', 1) + '+git' + revision
    if not re.fullmatch('[0-9][A-Za-z0-9.+~:-]*', version): raise ValueError('Invalid Debian version')
    if stage.exists(): raise ValueError('Preserve an existing package stage')
    runtime_id = revision + '-' + digest(payload / 'bin/xdnote')[:12]
    runtime = stage / 'opt/xdnote/versions' / runtime_id
    shutil.copytree(payload, runtime, symlinks=True)
    helper = payload / 'bin/xdnote-update-helper'
    if helper.is_symlink() or not helper.is_file(): raise ValueError('Independent helper missing')
    libexec = stage / 'usr/libexec'; libexec.mkdir(parents=True)
    shutil.copy2(helper, libexec / helper.name)
    marker = runtime / 'share/xdnote-install.json'; marker.parent.mkdir(exist_ok=True)
    marker.write_text(json.dumps({'schema_version': 1, 'product': 'xdnote', 'package_type': 'deb', **identity}) + '\n')
    binary = stage / 'usr/bin'; binary.mkdir(parents=True)
    launcher = binary / 'xdnote'; launcher.write_text('#!/bin/sh\nexec "/opt/xdnote/versions/' + runtime_id + '/bin/xdnote" "$@"\n'); launcher.chmod(0o755)
    share = stage / 'usr/share'; (share / 'applications').mkdir(parents=True)
    (share / 'applications/xdnote.desktop').write_text('[Desktop Entry]\nType=Application\nName=xdnote\nExec=/usr/bin/xdnote -- %f\nIcon=xdnote\nTerminal=false\nCategories=Office;TextEditor;\nMimeType=text/markdown;\n')
    icons = share / 'icons/hicolor/scalable/apps'; icons.mkdir(parents=True)
    shutil.copy2(ROOT / 'design/assets/ui-v2/brand/app-icon.svg', icons / 'xdnote.svg')
    policy = share / 'polkit-1/actions'; policy.mkdir(parents=True)
    shutil.copy2(ROOT / 'apps/desktop/packaging/linux/org.xdnote.update.policy', policy / 'org.xdnote.update.policy')
    tmpfiles = stage / 'usr/lib/tmpfiles.d'; tmpfiles.mkdir(parents=True)
    (tmpfiles / 'xdnote.conf').write_text('d /run/xdnote 0755 root root -\nf /run/xdnote/usage.lock 0644 root root -\n')
    packages = {'libc6', 'libstdc++6', 'libgcc-s1', 'libgl1', 'libegl1', 'libfontconfig1', 'libfreetype6', 'libx11-6', 'libx11-xcb1',
                'libxcb1', 'libxcb-cursor0', 'libxcb-icccm4', 'libxcb-image0', 'libxcb-keysyms1', 'libxcb-render-util0', 'libxcb-shape0',
                'libxcb-xinerama0', 'libxcb-xkb1', 'libxkbcommon0', 'libxkbcommon-x11-0', 'libdbus-1-3', 'libssl3', 'libzstd1', 'zlib1g',
                'desktop-file-utils', 'shared-mime-info', 'xdg-utils', 'util-linux', 'policykit-1', 'apt', 'ca-certificates'}
    # Resolve every host-native library used by this exact deployment to its
    # owning Ubuntu package; fixed supplemental packages cover dynamic plugins.
    for item in audit['imports']:
        if item.get('resolution') != 'host Linux system library': continue
        path = item.get('resolved', '')
        if path:
            packages.update(system_packages(path))
    document = share / 'doc/xdnote'; document.mkdir(parents=True)
    shutil.copy2(ROOT / 'LICENSE', document / 'copyright')
    control = stage / 'DEBIAN'; control.mkdir()
    dependencies = ', '.join('libc6 (>= 2.35)' if p == 'libc6' else p for p in sorted(packages))
    (control / 'control').write_text(f'Package: xdnote\nVersion: {version}\nArchitecture: amd64\nMaintainer: xiaodaozhi\nSection: editors\nPriority: optional\nX-Xdnote-Settings-Schema: 1\nDepends: {dependencies}\nDescription: xdnote Markdown reader and editor\n Development package; final release acceptance remains separate.\n')
    hook = (ROOT / 'apps/desktop/packaging/linux/package-hook.sh').read_text()
    for name in ('preinst', 'prerm', 'postinst', 'postrm'):
        path = control / name; path.write_text(hook.replace('hook=$1\nshift', 'hook=' + name)); path.chmod(0o755)
    return {'ok': True, 'version': version, 'runtimeId': runtime_id, 'buildIdentity': audit['buildIdentity'], 'releaseReady': False}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('root','audit','output','report'): p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); output = build_output(a.output)
    if output.exists(): p.error('Preserve an existing package')
    result = package_tree(build_output(a.root), json.loads(a.audit.read_bytes()), output.with_suffix('.stage'))
    subprocess.run(['/usr/bin/dpkg-deb', '--root-owner-group', '--build', str(output.with_suffix('.stage')), str(output)], check=True)
    result.update(sha256=digest(output), file=output.name); build_output(a.report).write_text(json.dumps(result, indent=2) + '\n')
