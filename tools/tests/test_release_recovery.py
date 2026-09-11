"""Public release checks tolerate propagation failures without replacing assets."""
import copy
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


class ChannelPromotion(unittest.TestCase):
    def test_stable_preserves_preview_and_old_identities(self):
        from tools.windows.publish_sdk import promote_index
        index = {'stable': None, 'preview': '0.16.1', 'versions': {'0.16.1': {'sha256': 'old'}}}
        entry = {'version': '0.16.2', 'entry': {'sha256': 'new'}, 'manager': {'build_id': 'new'},
                 'installer': {'preview': False, 'version': '0.16.2', 'code_signing': 'unsigned'}}
        promote_index(index, entry)
        self.assertEqual(index['stable'], '0.16.2')
        self.assertEqual(index['preview'], '0.16.1')
        self.assertEqual(index['versions']['0.16.1'], {'sha256': 'old'})
        snapshot = copy.deepcopy(index)
        promote_index(index, entry)
        self.assertEqual(index, snapshot)
        with self.assertRaises(SystemExit):
            promote_index(index, {**entry, 'entry': {'sha256': 'replaced'}})
        self.assertEqual(index, snapshot)
        with self.assertRaises(SystemExit):
            promote_index(index, {**entry, 'version': '0.16.0'})
        self.assertEqual(index, snapshot)

    def test_old_preview_does_not_replace_new_stable_installer(self):
        from tools.windows.publish_sdk import promote_index
        index = {'stable': '0.16.2', 'preview': None, 'versions': {},
                 'windows_installer': {'version': '0.16.2'}, 'manager': {'build_id': 'stable'}}
        promote_index(index, {'version': '0.16.1', 'entry': {}, 'manager': {},
                              'installer': {'preview': True, 'version': '0.16.1'}})
        self.assertEqual(index['windows_installer']['version'], '0.16.2')
        self.assertEqual(index['manager']['build_id'], 'stable')


if __name__ == '__main__':
    unittest.main()
