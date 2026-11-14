"""Shared, bounded installer input handling; never searches another checkout."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def within(path, root):
    return Path(path).resolve().is_relative_to(Path(root).resolve())


def build_output(path):
    path = Path(path).absolute()
    cursor = path
    while cursor != cursor.parent:
        if cursor.is_symlink() or cursor.is_junction():
            raise ValueError('Build paths cannot pass through links')
        cursor = cursor.parent
    path = path.resolve()
    if not within(path, ROOT / 'build') or path == ROOT / 'build':
        raise ValueError('Use a dedicated output under this project build/')
    return path
