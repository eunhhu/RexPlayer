"""Regression checks for historical evidence; these do not boot Android."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('evidence', ROOT / 'proof/validate_evidence.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)


class EvidenceTests(unittest.TestCase):
    def fixture(self, text):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / 'fixture.txt'
        path.write_text(text, encoding='utf-8')
        return path

    def test_committed_baseline(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            evidence.main()
        self.assertEqual(output.getvalue(), 'EVIDENCE_VALIDATION=PASS\n')

    def test_key_values(self):
        self.assertEqual(evidence.parse_key_values(self.fixture('A=one=two\nB=three\n')),
                         {'A': 'one=two', 'B': 'three'})

    def test_invalid_key_values(self):
        for text in ('A=one\nA=two\n', 'missing separator\n'):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                evidence.parse_key_values(self.fixture(text))

    def test_missing_key_value_file(self):
        with self.assertRaises(SystemExit):
            evidence.parse_key_values(Path('/nonexistent-rex-evidence-fixture'))

    def test_matrix_rejects_incomplete_duplicate_or_bad_verdict(self):
        original = (evidence.EVIDENCE / 'detection-matrix-result.tsv').read_text()
        lines = original.splitlines()
        variations = ['', '\n'.join(lines[:-1]), original + lines[1] + '\n',
                      original.replace('FAIL\t', 'CLEAN\t', 1),
                      original.replace('VERDICT\tCHECK\tEVIDENCE', 'wrong header')]
        for text in variations:
            with self.subTest(text=text[:70]), self.assertRaises(SystemExit):
                evidence.validate_matrix(self.fixture(text))

    def test_matrix_rejects_different_baseline_counts(self):
        text = (evidence.EVIDENCE / 'detection-matrix-result.tsv').read_text()
        with self.assertRaises(SystemExit):
            evidence.validate_matrix(self.fixture(text.replace('FAIL\t', 'PASS\t', 1)))

    def test_numeric_sequence_rejects_missing_reordered_or_extra_events(self):
        lines = ['EVENT type=%d code=%d value=%d' % event
                 for event in evidence.EXPECTED_NUMERIC_EVENTS]
        evidence.validate_numeric_log(self.fixture('\n'.join(lines)))
        for changed in (lines[:-1], lines + [lines[-1]], list(reversed(lines))):
            with self.subTest(changed=changed), self.assertRaises(SystemExit):
                evidence.validate_numeric_log(self.fixture('\n'.join(changed)))

    def test_android_sequence_rejects_wrong_value(self):
        text = (evidence.EVIDENCE / 'android-getevent.log').read_text()
        with self.assertRaises(SystemExit):
            evidence.validate_getevent(self.fixture(text.replace('0000002a', '0000002b')))

    def test_inputreader_requires_named_device_source_and_type(self):
        text = (evidence.EVIDENCE / 'android-inputreader.txt').read_text()
        for old, new in [('Input Reader State', 'No Reader'),
                         ('RexPlayer Virtual Multi-Touch Proof', 'Other Device'),
                         ('Sources: TOUCHSCREEN', 'Sources: MOUSE'),
                         ('DeviceType: TOUCH_SCREEN', 'DeviceType: POINTER')]:
            with self.subTest(old=old), self.assertRaises(SystemExit):
                evidence.validate_inputreader(self.fixture(text.replace(old, new)))

    def test_summary_requires_every_gate(self):
        lines = (evidence.EVIDENCE / 'android-input-summary.txt').read_text().splitlines()
        required = {'PRODUCER_EXIT', 'PRODUCER_PASS', 'GETEVENT_EVENT_LINES',
                    'GETEVENT_SEQUENCE', 'INPUTREADER_REGISTERED', 'TOUCHSCREEN_SOURCE',
                    'TOUCHSCREEN_DEVICE_TYPE', 'RESULT'}
        for key in required:
            changed = [line for line in lines if not line.startswith(key + '=')]
            with self.subTest(key=key), self.assertRaises(SystemExit):
                evidence.validate_android_summary(self.fixture('\n'.join(changed)))


if __name__ == '__main__':
    unittest.main()
