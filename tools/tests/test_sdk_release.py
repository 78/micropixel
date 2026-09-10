"""Verify SDK contents, determinism and the standalone CLI contract."""
import hashlib
import io
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

from tools import build_sdk_release as packager

ROOT = Path(__file__).resolve().parents[2]


class SdkArchive(unittest.TestCase):
    def test_archive_is_deterministic_and_standalone(self):
        metadata, first = packager.build(ROOT)
        self.assertEqual(packager.build(ROOT), (metadata, first))
        self.assertEqual(hashlib.sha256(first).hexdigest(), metadata['sha256'])
        with tempfile.TemporaryDirectory() as temporary, tarfile.open(fileobj=io.BytesIO(first)) as archive:
            names = archive.getnames()
            prefix = 'micropixel-sdk-' + metadata['version'] + '/'
            self.assertTrue(all(name.startswith(prefix) for name in names))
            self.assertFalse(any('/coastline/' in name or '/build/' in name or '/.env' in name for name in names))
            self.assertIn(prefix + 'guest/sdk/GRAPHICS.md', names)
            self.assertIn(prefix + 'LICENSE', names)
            self.assertIn(prefix + 'libexec/build_app_bundle.py', names)
            self.assertEqual(archive.getmember(prefix + 'micropixel').mode, 0o755)
            archive.extractall(temporary, filter='data')
            cli = Path(temporary) / prefix / 'micropixel'
            version = subprocess.check_output([sys.executable, str(cli), '--version'], text=True)
            self.assertIn(metadata['version'], version)
            project = Path(temporary) / '中文 game'
            subprocess.run([sys.executable, str(cli), 'init', str(project), '--app-id', 'test.sdk.hello'], check=True, capture_output=True)
            self.assertTrue((project / 'app.json').is_file())
            contents = cli.read_text(encoding='utf-8')
            self.assertNotIn(packager.MARKER, contents)

    def test_existing_version_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            metadata, data = packager.build(ROOT)
            destination = Path(temporary) / metadata['archiveName']
            destination.write_bytes(b'existing release')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/build_sdk_release.py'),
                                     '--output-dir', temporary], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Refusing to replace', result.stderr)
            self.assertEqual(destination.read_bytes(), b'existing release')


if __name__ == '__main__':
    unittest.main()
