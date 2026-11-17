"""Read the selected distribution policy without declaring release readiness."""
import json

from installer_common import ROOT


def current_policy():
    value = json.loads((ROOT / 'tools/release-policy.json').read_text(encoding='utf-8'))
    if value.get('schemaVersion') != 1:
        raise ValueError('Unsupported distribution policy schema')
    return value
