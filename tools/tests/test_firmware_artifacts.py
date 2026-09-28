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


class FirmwareArtifacts(unittest.TestCase):
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
