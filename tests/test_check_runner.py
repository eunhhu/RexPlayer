import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('source_checks', ROOT / 'scripts/check.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CheckRunnerTests(unittest.TestCase):
    def test_success(self):
        result = module.run('success', [sys.executable, '-c', 'print("done")'])
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['output'], 'done\n')

    def test_failure_keeps_output_and_exit(self):
        result = module.run('failure', [sys.executable, '-c', 'import sys; print("failed"); sys.exit(7)'])
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['exit_code'], 7)
        self.assertEqual(result['output'], 'failed\n')

    def test_missing_tool_is_not_passed(self):
        result = module.run('missing', ['rexplayer-nonexistent-test-command'])
        self.assertEqual(result['status'], 'BLOCKED')

    def test_timeout_is_failure(self):
        result = module.run('timeout', [sys.executable, '-c', 'import time; time.sleep(30)'], timeout=0.05)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('timeout', result['reason'])


if __name__ == '__main__':
    unittest.main()
