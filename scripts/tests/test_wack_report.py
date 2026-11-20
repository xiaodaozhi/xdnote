from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wack_report import summarize


class WackReportTests(unittest.TestCase):
    def report(self, *, dpi='PASS', mandatory='PASS'):
        test_results = [('App manifest', 'FALSE', mandatory), ('Blocked executables', 'TRUE', 'FAIL')]
        if dpi is not None:
            test_results.append(('DPIAwarenessValidation', 'FALSE', dpi))
        tests = []
        for name, optional, result in test_results:
            messages = ''
            if name == 'Blocked executables':
                messages = ('<MESSAGES>'
                            '<MESSAGE TEXT="File Qt6Core.dll contains a reference to kernel32.dll!CreateProcessW"/>'
                            '<MESSAGE TEXT="File xdnote.exe contains a blocked executable reference to &quot;bash&quot;."/>'
                            '</MESSAGES>')
            tests.append(f'<TEST NAME="{name}" OPTIONAL="{optional}">{messages}<RESULT>{result}</RESULT></TEST>')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'wack.xml'
            path.write_text('<REPORT><REQUIREMENTS><REQUIREMENT>' + ''.join(tests)
                            + '</REQUIREMENT></REQUIREMENTS></REPORT>', encoding='utf-8')
            return summarize(path)

    def test_records_optional_failure_without_counting_it_as_mandatory(self):
        report = self.report()
        self.assertTrue(report['passed'])
        self.assertEqual(report['checks'][1]['result'], 'FAIL')
        self.assertEqual(report['checks'][1]['messages'], [
            'File Qt6Core.dll contains a reference to kernel32.dll!CreateProcessW',
            'File xdnote.exe contains a blocked executable reference to "bash".',
        ])
        self.assertEqual(report['mandatoryFailures'], [])

    def test_requires_dpi_pass_and_no_mandatory_failures(self):
        for dpi, mandatory in [('WARNING', 'PASS'), (None, 'PASS'), ('PASS', 'FAIL')]:
            with self.subTest(dpi=dpi, mandatory=mandatory):
                self.assertFalse(self.report(dpi=dpi, mandatory=mandatory)['passed'])
