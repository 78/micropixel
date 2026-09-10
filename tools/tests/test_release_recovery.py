"""Public release checks tolerate propagation failures without replacing assets."""
import io
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import patch
from tools.windows import verify_installed_sdk as verification


class PublicDownload(unittest.TestCase):
    def test_transient_missing_asset_is_retried(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'asset.zip'
            missing = urllib.error.HTTPError('https://example.org/asset', 404, 'not ready', {}, None)
            with patch.object(verification.urllib.request, 'urlopen', side_effect=[missing, io.BytesIO(b'verified bytes')]) as request, patch.object(verification.time, 'sleep') as sleep:
                verification.download_public('https://example.org/asset', path)
            self.assertEqual(path.read_bytes(), b'verified bytes')
            self.assertEqual(request.call_count, 2)
            sleep.assert_called_once_with(2)

    def test_permanent_denial_is_not_retried(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'asset.zip'
            denied = urllib.error.HTTPError('https://example.org/asset', 403, 'denied', {}, None)
            with patch.object(verification.urllib.request, 'urlopen', side_effect=denied) as request, patch.object(verification.time, 'sleep') as sleep:
                with self.assertRaises(urllib.error.HTTPError):
                    verification.download_public('https://example.org/asset', path)
            request.assert_called_once()
            sleep.assert_not_called()
            self.assertFalse(path.exists())


if __name__ == '__main__':
    unittest.main()
