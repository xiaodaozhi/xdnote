"""Inspect product imports without loading the binary or using desktop input."""
import argparse
import json
from pathlib import Path
import platform
import re
import subprocess

from deployment_inventory import pe_imports
from installer_common import build_output


def check(executable, system=None):
    system = system or platform.system()
    if system == 'Windows':
        dependencies = pe_imports(executable)
    else:
        command = ['otool', '-L', str(executable)] if system == 'Darwin' else ['readelf', '-d', str(executable)]
        dependencies = subprocess.check_output(command, text=True, timeout=30).splitlines()
    if any(re.search(r'(?i)(?:qt6|Qt)(?:quick|qml)', name) for name in dependencies):
        raise ValueError('Product imports an experimental Quick/QML module')
    return {'ok': True, 'scope': 'product direct imports; deployed transitive closure checked separately',
            'executable': str(executable), 'dependencies': dependencies}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(build_output(args.executable)), ensure_ascii=False))
