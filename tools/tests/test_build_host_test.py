"""Regression coverage for compilation cache invalidation using the real C compiler."""

import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.tests.build_host_test import build


class HostTestBuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="host test cache ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.header = self.root / "value.h"
        self.header.write_text("#define VALUE 0\n")
        self.source = self.root / "test.c"
        self.source.write_text('#include "value.h"\nint main(void) { return VALUE; }\n')
        self.output = self.root / "test"
        self.command = [os.environ.get("CC", "/usr/bin/clang"), "-std=c17", str(self.source), "-o", str(self.output)]

    def build(self, command=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return build(command or self.command)

    def test_reuses_binary_but_rebuilds_missing_output_and_changed_flags(self):
        self.assertTrue(self.build())
        self.assertFalse(self.build())
        self.output.unlink()
        self.assertTrue(self.build())
        self.assertTrue(self.build(self.command + ["-DNEW_FLAG=1"]))
        self.assertFalse(self.build(self.command + ["-DNEW_FLAG=1"]))

    def test_header_content_changes_even_with_preserved_timestamp(self):
        self.build()
        before = self.header.stat()
        self.header.write_text("#define VALUE 7\n")
        os.utime(self.header, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertTrue(self.build())
        self.assertEqual(subprocess.run([str(self.output)]).returncode, 7)
        self.source.write_text("int main(void) { return 0; }\n")
        self.assertTrue(self.build())

    def test_new_shadowing_header_is_detected(self):
        includes = self.root / "includes"
        includes.mkdir()
        self.header.rename(includes / "value.h")
        command = self.command + ["-I", str(includes)]
        self.build(command)
        self.header.write_text("#define VALUE 9\n")
        self.assertTrue(self.build(command))
        self.assertEqual(subprocess.run([str(self.output)]).returncode, 9)

    def test_environment_and_force_rebuild(self):
        self.build()
        with patch.dict(os.environ, {"CPATH": str(self.root)}):
            self.assertTrue(self.build())
            self.assertFalse(self.build())
        with patch.dict(os.environ, {"HOST_TEST_REBUILD": "1"}):
            self.assertTrue(self.build())
            self.assertTrue(self.build())

    def test_failed_link_cannot_publish_cache_entry(self):
        self.build()
        self.source.write_text("int missing(void); int main(void) { return missing(); }\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.build()
        self.assertFalse(self.output.with_name("test.build.json").exists())
        self.source.write_text("int main(void) { return 0; }\n")
        self.assertTrue(self.build())

    def test_multiple_translation_units_track_all_headers(self):
        second_header = self.root / "second.h"
        second_header.write_text("#define SECOND 3\n")
        second_source = self.root / "second.c"
        second_source.write_text('#include "second.h"\nint second(void) { return SECOND; }\n')
        self.source.write_text("int second(void); int main(void) { return second(); }\n")
        command = self.command + [str(second_source)]
        self.assertTrue(self.build(command))
        self.assertFalse(self.build(command))
        second_header.write_text("#define SECOND 4\n")
        self.assertTrue(self.build(command))
        self.assertEqual(subprocess.run([str(self.output)]).returncode, 4)


if __name__ == "__main__":
    unittest.main()
