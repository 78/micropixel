# SPDX-License-Identifier: Apache-2.0
"""Check dependency preparation failures without contacting remote services."""
import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'prepare_deps', Path(__file__).resolve().parents[1] / 'prepare_deps.py')
DEPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DEPS)


class DependencyPreparation(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.archive = self.root / 'source.tar.gz'
        source = self.root / 'source'
        source.mkdir()
        (source / 'input.c').write_bytes(b'pinned source')
        with tarfile.open(self.archive, 'w:gz') as archive:
            archive.add(source, arcname='source')
        self.target = self.root / 'deps'
        self.pin = {
            'url': self.archive.as_uri(),
            'archive_sha256': hashlib.sha256(self.archive.read_bytes()).hexdigest(),
            'verify_files': {'input.c': hashlib.sha256(b'pinned source').hexdigest()},
        }

    def test_prepare_and_check_preserve_existing_source(self):
        DEPS.prepare('fixture', self.pin, self.target, False)
        DEPS.prepare('fixture', self.pin, self.target, True)
        path = self.target / 'fixture' / 'input.c'
        path.write_bytes(b'local changes')
        with self.assertRaises(ValueError):
            DEPS.prepare('fixture', self.pin, self.target, False)
        self.assertEqual(path.read_bytes(), b'local changes')

    def test_check_missing_dependency_does_not_download(self):
        with self.assertRaises(ValueError):
            DEPS.prepare('fixture', self.pin, self.target, True)
        self.assertFalse(self.target.exists())

    def test_bad_archive_checksum_never_installs(self):
        self.pin['archive_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            DEPS.prepare('fixture', self.pin, self.target, False)
        self.assertFalse((self.target / 'fixture').exists())

    def test_unexpected_source_never_installs(self):
        self.pin['verify_files']['input.c'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'source mismatch'):
            DEPS.prepare('fixture', self.pin, self.target, False)
        self.assertFalse((self.target / 'fixture').exists())


if __name__ == '__main__':
    unittest.main()
