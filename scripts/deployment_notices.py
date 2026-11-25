"""Prepare the third-party notice for the installed directory layout."""
import re
from pathlib import PurePosixPath

from installer_common import ROOT, digest
from qt_source_access import project_source


SOURCE = ROOT / 'THIRD_PARTY_NOTICES.md'
LINK = re.compile(r'\]\(([^)\s]+)\)')


def installed_notice(licenses, identity):
    source = project_source(identity)
    revision = source['revision'] if source else 'main'
    web_root = f'https://github.com/xiaodaozhi/xdnote/blob/{revision}/'

    def link(match):
        target = match[1]
        if target.startswith(('https://', 'http://', '#')):
            return match[0]
        path, separator, fragment = target.partition('#')
        relative = PurePosixPath(path)
        if relative.is_absolute() or '..' in relative.parts or not (ROOT / relative).is_file():
            raise ValueError('Unknown third-party notice link: ' + target)
        if path.startswith('LICENSES/'):
            destination = 'project-and-source-dependencies/' + path.removeprefix('LICENSES/')
        elif path == 'docs/engineering/qt-library-replacement.md':
            destination = 'qt/REPLACEMENT.md'
        else:
            return '](' + web_root + target + ')'
        if not (licenses / destination).is_file():
            raise ValueError('Third-party notice material is missing: ' + destination)
        return '](' + destination + separator + fragment + ')'

    return LINK.sub(link, SOURCE.read_text(encoding='utf-8')).encode('utf-8')


def collect(licenses, identity):
    (licenses / SOURCE.name).write_bytes(installed_notice(licenses, identity))


def verify(path, identity):
    current = path.read_bytes()
    # Older sealed payloads contain the original repository notice bytes.
    if current != SOURCE.read_bytes() and current != installed_notice(path.parent, identity):
        raise ValueError('Distributed third-party notice changed')
    return digest(SOURCE)
