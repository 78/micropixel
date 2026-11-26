from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SdAppStorageBackend(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'requires native g++ with C++23 std::expected')
    def test_production_backend_with_fatfs_faults(self):
        root = Path(__file__).resolve().parents[2]
        tests = root / 'tools/tests'
        with tempfile.TemporaryDirectory(prefix='mpx-sd-test-') as directory:
            binary = Path(directory) / 'sd-app-storage-test'
            subprocess.run([
                'g++', '-std=c++23', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-I' + str(tests / 'sd_storage_stubs'), '-I' + str(root / 'common'),
                '-I' + str(root.parents[1] / 'firmware/espressif/main'),
                str(root / 'common/sd_app_storage.cpp'), str(tests / 'sd_app_storage_test.cpp'),
                '-o', str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True)
