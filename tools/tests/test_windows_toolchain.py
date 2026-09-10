"""Windows path regressions, runnable on POSIX and Windows."""
import tempfile
import unittest
from pathlib import Path

from tools.tests.test_micropixel_cli import CLI


class WindowsDependencies(unittest.TestCase):
    def test_drive_paths_keep_directory_separators_and_escaped_spaces(self):
        with tempfile.TemporaryDirectory() as temporary:
            dep = Path(temporary) / 'object.d'
            dep.write_text('object: C:\\Users\\dev\\game\\main.cpp C:/中文/my\\ game/header.hpp\n', encoding='utf-8')
            paths = CLI._read_guest_depfile(dep)
            self.assertEqual(set(paths), {
                Path(r'C:\Users\dev\game\main.cpp').absolute(),
                Path('C:/中文/my game/header.hpp').absolute(),
            })

    def test_make_escapes_and_continuations_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            dep = Path(temporary) / 'object.d'
            dep.write_text('object: /tmp/a\\#b.hpp \\\n /tmp/a$$b.hpp\n')
            self.assertEqual(set(CLI._read_guest_depfile(dep)), {
                Path('/tmp/a#b.hpp').absolute(), Path('/tmp/a$b.hpp').absolute(),
            })


if __name__ == '__main__':
    unittest.main()
