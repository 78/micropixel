"""Machine-output tests execute the CLI exactly as an AI client would."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class JsonOutput(unittest.TestCase):
    def call(self, *args, env=None):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/micropixel'), *map(str, args)],
                                capture_output=True, text=True, encoding='utf-8', env=env)
        value = json.loads(result.stdout)  # Reject any progress text mixed into stdout.
        self.assertEqual(value['schema_version'], 1)
        self.assertEqual(value['ok'], result.returncode == 0)
        return result, value

    def test_init_success_and_existing_project_failure_are_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / '中文 game'
            result, value = self.call('init', root, '--app-id', 'test.json.hello', '--json')
            self.assertEqual(result.returncode, 0)
            self.assertEqual(value['result']['project'], str(root.resolve()))
            self.assertIn('Created', result.stderr)
            result, value = self.call('init', root, '--json')
            self.assertEqual(result.returncode, 1)
            self.assertEqual(value['code'], 'execution_failed')

    def test_parse_failure_and_follow_rejection_have_no_side_effect(self):
        result, value = self.call('build', '--invalid-option', '--json')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(value['code'], 'invalid_arguments')
        result, value = self.call('run', '--json')
        self.assertEqual(result.returncode, 2)
        self.assertIn('--no-follow', value['error']['message'])

    def test_package_requires_an_explicit_target_as_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'game'
            self.call('init', root, '--app-id', 'test.json.hello', '--json')
            result, value = self.call('package', root, '--json')
            self.assertEqual(result.returncode, 3)
            self.assertEqual(value['code'], 'input_required')
            self.assertIn('--aot-target', value['error']['message'])

    def test_missing_compiler_is_environment_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'game'
            self.call('init', root, '--app-id', 'test.json.hello', '--json')
            env = {**os.environ, 'WASI_CLANGXX': str(root / 'absent-clang++')}
            result, value = self.call('build', root, '--json', env=env)
            self.assertEqual(result.returncode, 4)
            self.assertEqual(value['code'], 'environment_unavailable')
            self.assertEqual(value['result']['artifacts'], [])


if __name__ == '__main__':
    unittest.main()
