import json
import os
import subprocess
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.ci.verify_firmware_sdk import changed_inputs
from tools.ci import firmware_artifacts


class FirmwareSdkInputsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        for name in ('guest/sdk/api.hpp', 'guest/runtime/start.cpp', 'guest/abi/wire.h',
                     'tools/micropixel', 'tools/build_app_bundle.py', 'tools/generate_localization.py',
                     'tools/analyze_sfx.py', 'guest/sdk/README.md', 'guest/apps/maze/game.cpp'):
            self.write(name, 'baseline\n')
        self.commit()
        self.git('tag', 'sdk')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self):
        self.git('add', '-A')
        self.git('commit', '-qm', 'fixture')

    def test_docs_and_factory_apps_do_not_change_sdk_build_inputs(self):
        self.git('mv', 'guest/sdk/README.md', 'guest/sdk/README.zh-CN.md')
        self.write('guest/apps/maze/game.cpp', 'new application\n')
        self.commit()
        self.assertEqual(changed_inputs(self.root, 'sdk'), [])

    def test_runtime_abi_headers_and_packaging_tools_are_checked(self):
        names = ['guest/sdk/api.hpp', 'guest/runtime/start.cpp', 'guest/abi/wire.h', 'tools/micropixel',
                 'tools/build_app_bundle.py', 'tools/generate_localization.py', 'tools/analyze_sfx.py']
        for name in names:
            self.write(name, 'changed\n')
        self.commit()
        self.assertEqual(set(changed_inputs(self.root, 'sdk')), set(names))

    def test_added_deleted_and_renamed_build_inputs_are_checked(self):
        (self.root / 'guest/sdk/api.hpp').unlink()
        self.write('guest/sdk/new.hpp', 'new header\n')
        self.git('mv', 'guest/runtime/start.cpp', 'guest/runtime/entry.cpp')
        self.commit()
        self.assertEqual(set(changed_inputs(self.root, 'sdk')),
                         {'guest/sdk/api.hpp', 'guest/sdk/new.hpp', 'guest/runtime/start.cpp', 'guest/runtime/entry.cpp'})


class FirmwareGuestToolchainTest(unittest.TestCase):
    def test_checkout_apps_use_verified_toolchain_not_installed_example_headers(self):
        setup = {'ok': True, 'result': {'sdk_version': '0.17.0', 'toolchain_id': 'verified-fixture',
                 'paths': {'WASI_SDK_PATH': '/verified/wasi', 'WAMRC': '/verified/riscv',
                           'XTENSA_WAMRC': '/verified/xtensa', 'sdk': '/installed/older-examples'}}}
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(firmware_artifacts.subprocess, 'check_output', return_value=json.dumps(setup)), \
                patch.object(firmware_artifacts, 'run') as run, \
                patch.object(firmware_artifacts, 'versions', return_value=('0.8.3', '0.17.0')), \
                patch.dict(os.environ, {'GITHUB_SHA': 'a' * 40, 'WAMRC': '/unverified/compiler'}):
            output = Path(temporary)
            firmware_artifacts.guest(output, 'verified-launcher')
            packages = [call for call in run.call_args_list if 'package' in call.args]
            self.assertEqual(len(packages), 2 * len(firmware_artifacts.SOURCES['guest_apps']))
            for call in packages:
                self.assertEqual(call.args[1], firmware_artifacts.ROOT / 'tools/micropixel')
                self.assertTrue(call.args[3].is_relative_to(firmware_artifacts.ROOT / 'guest/apps'))
                self.assertEqual(call.kwargs['env']['WAMRC'], '/verified/riscv')
                self.assertEqual(call.kwargs['env']['XTENSA_WAMRC'], '/verified/xtensa')
            self.assertEqual(json.loads((output / 'manifest.json').read_text())['toolchain_id'], 'verified-fixture')

    def test_wrong_sdk_version_is_rejected_before_build(self):
        setup = {'ok': True, 'result': {'sdk_version': '0.16.0'}}
        with patch.object(firmware_artifacts.subprocess, 'check_output', return_value=json.dumps(setup)), \
                patch.object(firmware_artifacts, 'versions', return_value=('0.8.3', '0.17.0')), \
                patch.object(firmware_artifacts, 'run') as run:
            with self.assertRaises(ValueError):
                firmware_artifacts.guest(Path('unused'), 'verified-launcher')
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
