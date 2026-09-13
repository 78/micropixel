import subprocess
import tempfile
from pathlib import Path
import unittest

from tools.ci.verify_firmware_sdk import changed_inputs


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


if __name__ == '__main__':
    unittest.main()
