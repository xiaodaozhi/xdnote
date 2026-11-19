"""Read the Windows App Certification Kit results recorded in its XML report."""

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def summarize(path):
    report = ET.parse(path).getroot()
    checks = []
    names = set()
    for test in report.findall('./REQUIREMENTS/REQUIREMENT/TEST'):
        name = test.get('NAME')
        optional = test.get('OPTIONAL')
        result = test.findtext('RESULT')
        if not name or name in names or optional not in ('TRUE', 'FALSE') or not result:
            raise ValueError('Incomplete or duplicate WACK check')
        names.add(name)
        checks.append({'name': name, 'optional': optional == 'TRUE', 'result': result})
    if report.tag != 'REPORT' or not any(not check['optional'] for check in checks):
        raise ValueError('WACK report lacks mandatory checks')

    mandatory_failures = [check['name'] for check in checks
                          if not check['optional'] and check['result'] == 'FAIL']
    dpi = next((check['result'] for check in checks if check['name'] == 'DPIAwarenessValidation'), None)
    return {
        'passed': not mandatory_failures and dpi == 'PASS',
        'mandatoryFailures': mandatory_failures,
        'dpiAwareness': dpi,
        'checks': checks,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.report)))
