# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class FlashStorage(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("BL_SDK_BASE"), "requires the pinned Bouffalo SDK in BL_SDK_BASE")
    @unittest.skipUnless(shutil.which("gcc") and shutil.which("g++"), "requires native gcc and C++23 g++")
    def test_remount_and_preserve_foreign_flash(self):
        tests = Path(__file__).resolve().parent
        platform = tests.parents[1]
        sdk = Path(os.environ["BL_SDK_BASE"])
        lfs = sdk / "components/fs/littlefs/littlefs"
        mtd = sdk / "components/utils/bflb_mtd/include"
        with tempfile.TemporaryDirectory(prefix="mpx-flash-test-") as tmp:
            work = Path(tmp)
            objects = []
            for source in ("lfs", "lfs_util"):
                obj = work / (source + ".o")
                subprocess.run(
                    ["gcc", "-std=c11", "-DLFS_THREADSAFE", "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR",
                     "-c", str(lfs / (source + ".c")), "-o", str(obj)],
                    check=True,
                )
                objects.append(str(obj))
            binary = work / "flash-storage-test"
            subprocess.run(
                ["g++", "-std=c++23", "-Wall", "-Wextra", "-Werror", "-pthread", "-DLFS_THREADSAFE",
                 "-I" + str(tests / "flash_stubs"), "-I" + str(platform / "common"),
                 "-I" + str(platform / "common/include"), "-I" + str(lfs), "-I" + str(mtd),
                 "-I" + str(platform.parents[1] / "firmware/espressif/main"),
                 str(platform / "common/storage.cpp"), str(platform / "common/app_storage.cpp"),
                 str(tests / "flash_storage_test.cpp"), *objects, "-o", str(binary)],
                check=True,
            )
            for mode in ("write", "read", "foreign", "read-failure"):
                with self.subTest(mode=mode):
                    subprocess.run([str(binary), mode, str(work / "flash.bin")], check=True)
