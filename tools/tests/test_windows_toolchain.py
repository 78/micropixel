"""Windows path regressions, runnable on POSIX and Windows."""
import tempfile
import json
import signal
import subprocess
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from pathlib import Path

from tools.tests.test_micropixel_cli import CLI
from tools.manager import process_control


class WindowsConsoleInterrupts(unittest.TestCase):
    def test_wrapper_restores_signal_handler_after_spawn_failure(self):
        previous = signal.getsignal(signal.SIGINT)
        with patch.object(process_control.sys, 'platform', 'win32'):
            with patch.object(process_control.subprocess, 'run', side_effect=OSError('spawn failed')):
                with self.assertRaisesRegex(OSError, 'spawn failed'):
                    process_control.run_child(['missing'])
        self.assertIs(signal.getsignal(signal.SIGINT), previous)

    def test_non_windows_interrupt_is_not_swallowed(self):
        with patch.object(process_control.sys, 'platform', 'linux'):
            with patch.object(process_control.subprocess, 'run', side_effect=KeyboardInterrupt):
                with self.assertRaises(KeyboardInterrupt):
                    process_control.run_child(['child'])

    @unittest.skipUnless(sys.platform == 'win32', 'requires Windows console event broadcast')
    def test_repeated_ctrl_c_reaches_child_and_preserves_exit_code(self):
        fixture = Path(__file__).with_name('fixtures') / 'windows_console_interrupt.py'
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        for code in (0, 3):
            with self.subTest(exit_code=code), tempfile.TemporaryDirectory() as temporary:
                result = subprocess.run(
                    [sys.executable, '-X', 'utf8', str(fixture), '2', str(code), str(Path(temporary) / 'ready')],
                    creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup,
                    capture_output=True, text=True, encoding='utf-8', timeout=30,
                )
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertEqual(json.loads(result.stdout), {'interrupts': 2, 'exit_code': code})
                self.assertEqual(result.stderr, '')


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

    def test_usb_discovery_distinguishes_environment_and_missing_selection(self):
        with patch.object(CLI, 'usb_serial_ports', return_value=[]):
            with self.assertRaises(CLI.EnvironmentFailure):
                CLI.discover_usb_port(None)
        ports = [SimpleNamespace(device=name, vid=0x303A, pid=0x1001, product='MicroPixel') for name in ('COM4', 'COM5')]
        with patch.object(CLI, 'usb_serial_ports', return_value=ports):
            with self.assertRaises(CLI.InputRequired):
                CLI.discover_usb_port(None)
            self.assertEqual(CLI.discover_usb_port('COM5'), 'COM5')

    def test_wamrc_unicode_staging_preserves_output_on_failure(self):
        with tempfile.TemporaryDirectory(prefix='中文 game ') as temporary:
            root = Path(temporary)
            wasm, aot = root / '游戏.wasm', root / '游戏.aot'
            wasm.write_bytes(b'wasm input')
            aot.write_bytes(b'previous output')
            def compile(command, *, cwd, check):
                self.assertEqual(command[-3:], ['-o', 'module.aot', 'module.wasm'])
                self.assertEqual((cwd / 'module.wasm').read_bytes(), b'wasm input')
                (cwd / 'module.aot').write_bytes(b'new output')
            with patch.object(CLI.sys, 'platform', 'win32'), patch.object(CLI, '_run_process', side_effect=OSError('interrupted')):
                with self.assertRaises(OSError):
                    CLI._run_aot_compiler(['wamrc'], wasm, aot)
            self.assertEqual(aot.read_bytes(), b'previous output')
            with patch.object(CLI.sys, 'platform', 'win32'), patch.object(CLI, '_run_process', side_effect=compile):
                CLI._run_aot_compiler(['wamrc'], wasm, aot)
            self.assertEqual(aot.read_bytes(), b'new output')
            self.assertEqual(list(root.glob('.wamrc-*')), [])

    def test_make_escapes_and_continuations_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            dep = Path(temporary) / 'object.d'
            dep.write_text('object: /tmp/a\\#b.hpp \\\n /tmp/a$$b.hpp\n')
            self.assertEqual(set(CLI._read_guest_depfile(dep)), {
                Path('/tmp/a#b.hpp').absolute(), Path('/tmp/a$b.hpp').absolute(),
            })


if __name__ == '__main__':
    unittest.main()
