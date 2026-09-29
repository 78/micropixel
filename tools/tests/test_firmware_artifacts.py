import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from tools.ci.firmware_artifacts import (
    ROOT, SOURCES, app_store_mib, check_full_image, check_image, check_files, guest_store_sizes, inventory,
)
sys.path.insert(0, str(ROOT / 'tools/ci'))
from plan_pr_builds import select


class FirmwareArtifacts(unittest.TestCase):
    def selected_profiles(self, paths):
        return {entry['profile'] for entry in select(paths)}

    def test_pr_board_selection_and_s3_common_code(self):
        self.assertEqual(self.selected_profiles([
            'firmware/espressif/main/platform/boards/sensecap-watcher/battery_peripheral.cpp',
            'firmware/espressif/main/platform/boards/sensecap-watcher/board_power.hpp',
        ]), {'sensecap-watcher'})
        self.assertEqual(self.selected_profiles([
            'firmware/espressif/main/platform/boards/esp32-s3-box-3/platform.cpp',
        ]), {'esp-box-3'})
        self.assertEqual(self.selected_profiles([
            'firmware/espressif/main/platform/boards/esp32-s3-common/display.cpp',
        ]), {'esp-box-3', 'szpi-esp32s3', 'm5stack-cores3', 'sensecap-watcher'})

    def test_pr_shared_host_inputs_cover_all_release_boards(self):
        for path in ('firmware/espressif/main/platform/transports/log_output_lock.hpp',
                     'firmware/espressif/components/wasm-micro-runtime',
                     'firmware/espressif/main/platform/boards/new-board/platform.cpp',
                     'guest/abi/micropixel_abi.h', 'guest/sdk/symbols.hpp',
                     'tools/generate_builtin_fonts.py', 'tools/ci/firmware-sources.json',
                     '.github/workflows/pr-build.yml'):
            with self.subTest(path=path):
                self.assertEqual(self.selected_profiles([path]), set(SOURCES['profiles']))

    def test_pr_docs_and_guest_changes_do_not_require_host_builds(self):
        self.assertEqual(select(['README.md', 'docs/development/flashing.zh-CN.md',
                                 'firmware/espressif/main/README.zh-CN.md',
                                 'guest/apps/snake/main.cpp', 'guest/sdk/ui/layout.hpp',
                                 'tools/micropixel', 'tools/tests/test_guest_ui.cpp']), [])

    def test_pr_configs_and_wrappers_select_only_affected_profiles(self):
        self.assertEqual(self.selected_profiles(['tools/p4.sh']), {'metalio-claw4'})
        self.assertEqual(self.selected_profiles(['tools/s31.sh']), {'esp-mosaico'})
        self.assertEqual(self.selected_profiles(['firmware/espressif/sdkconfig.s3-watcher.defaults']),
                         {'sensecap-watcher'})
        self.assertEqual(self.selected_profiles(['firmware/espressif/sdkconfig.defaults']), set(SOURCES['profiles']))

    def test_empty_pr_plan_skips_compilation_with_valid_matrix_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'output'
            environment = dict(os.environ, GITHUB_OUTPUT=str(output), GITHUB_STEP_SUMMARY='')
            subprocess.run([sys.executable, str(ROOT / 'tools/ci/plan_pr_builds.py'), '--base', 'HEAD'],
                           env=environment, check=True, capture_output=True)
            values = dict(line.split('=', 1) for line in output.read_text().splitlines())
            self.assertEqual(values['has_hosts'], 'false')
            self.assertTrue(json.loads(values['hosts']))

    def test_store_capacity_comes_from_board_profile_not_chip(self):
        self.assertEqual(app_store_mib('sensecap-watcher'), 24)
        self.assertEqual(app_store_mib('szpi-esp32s3'), 8)
        self.assertEqual(guest_store_sizes('xtensa'), [8, 24])
        self.assertEqual(guest_store_sizes('riscv32-ilp32f'), [8, 24])

    def test_full_image_uses_declared_flash_capacity_and_preserves_ota(self):
        ota = b'verified OTA'
        full = bytearray(32 * 1024 * 1024)
        full[0x30000:0x30000 + len(ota)] = ota
        check_full_image(full, ota, {'flash_settings': {'flash_size': '32MB'}})
        with self.assertRaises(ValueError):
            check_full_image(full, ota, {'flash_settings': {'flash_size': '16MB'}})
        with self.assertRaises(ValueError):
            check_full_image(full, b'wrong OTA', {'flash_settings': {'flash_size': '32MB'}})

    def test_host_plan_covers_release_profiles_and_selects_watcher_rebuild(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'output'
            environment = dict(os.environ, GITHUB_OUTPUT=str(output), REUSE_RUN='', REBUILD_PROFILES='')
            subprocess.run([sys.executable, str(ROOT / 'tools/ci/plan_hosts.py')], env=environment, check=True)
            matrix = json.loads(output.read_text().split('=', 1)[1])
            self.assertEqual({entry['profile'] for entry in matrix}, set(SOURCES['profiles']))
            self.assertEqual(len(matrix), len(SOURCES['profiles']))
            output.unlink()
            environment.update(REUSE_RUN='123', REBUILD_PROFILES='sensecap-watcher')
            subprocess.run([sys.executable, str(ROOT / 'tools/ci/plan_hosts.py')], env=environment, check=True)
            matrix = json.loads(output.read_text().split('=', 1)[1])
            self.assertEqual([entry['profile'] for entry in matrix], ['sensecap-watcher'])
            self.assertEqual(matrix[0]['chip'], 'esp32s3')
            self.assertEqual(matrix[0]['wrapper'], 's3.sh build-host watcher')

    def test_wrong_chip_version_and_slot_overflow_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            p = Path(temporary) / 'app.bin'
            data = bytearray(256)
            data[0] = 0xE9
            struct.pack_into('<H', data, 12, 18)
            struct.pack_into('<I', data, 32, 0xABCD5432)
            data[48:53] = b'0.8.0'
            p.write_bytes(data)
            check_image(p, 'esp32p4', '0.8.0')
            for target, version in [('esp32s3', '0.8.0'), ('esp32p4', '0.7.7')]:
                with self.assertRaises(ValueError): check_image(p, target, version)
            p.write_bytes(data + b'\0' * 0x380000)
            with self.assertRaises(ValueError): check_image(p, 'esp32p4', '0.8.0')

    def test_changed_or_escaping_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'app.bin').write_bytes(b'validated')
            files = inventory(root)
            check_files(root, files)
            (root / 'app.bin').write_bytes(b'corrupted')
            with self.assertRaises(ValueError): check_files(root, files)
            with self.assertRaises(ValueError): check_files(root, {'../escape': files['app.bin']})
