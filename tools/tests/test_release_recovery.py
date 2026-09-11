"""Public release checks tolerate propagation failures without replacing assets."""
import copy
import io
import json
import subprocess
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


class ReleasePresentation(unittest.TestCase):
    def test_user_files_exclude_fixtures_and_route_support_separately(self):
        from tools.windows.release_assets import user_assets, asset_url, split_checksums
        entry = {'version': '1.2.3', 'installer': {'name': 'setup.exe'}, 'manager': {'name': 'manager.zip'},
                 'entry': {'url': 'https://example.org/sdk/sdk-manifest.json'}, 'verification_base': 'https://example.org/support'}
        self.assertNotIn('test-sdk-index.json', user_assets(entry))
        self.assertEqual(asset_url(entry, 'test-sdk-index.json'), 'https://example.org/support/test-sdk-index.json')
        self.assertEqual(asset_url(entry, 'setup.exe'), 'https://example.org/sdk/setup.exe')
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary).resolve()
            (out / 'setup.exe').write_bytes(b'installer')
            (out / 'test-sdk-index.json').write_bytes(b'tests')
            split_checksums(out, entry)
            self.assertNotIn('test-sdk-index', (out / 'sha256sums.txt').read_text())
            self.assertIn('test-sdk-index', (out / 'verification-sha256sums.txt').read_text())

    def test_legacy_verification_urls_still_work(self):
        from tools.windows.release_assets import asset_url
        entry = {'version': '1.2.3', 'installer': {'name': 'setup.exe'}, 'manager': {'name': 'manager.zip'},
                 'entry': {'url': 'https://example.org/sdk/sdk-manifest.json'}}
        self.assertEqual(asset_url(entry, 'test-sdk-index.json'), 'https://example.org/sdk/test-sdk-index.json')


class PublishLayout(unittest.TestCase):
    def test_draft_sends_fixtures_only_to_support_release(self):
        from tools.windows import publish_sdk
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary).resolve()
            entry = {'version': '1.2.3', 'installer': {'name': 'setup.exe', 'url': 'https://example.org/setup.exe', 'preview': False},
                     'manager': {'name': 'manager.zip'}, 'entry': {'url': 'https://example.org/sdk-manifest.json'}}
            (out / 'channel-entry.json').write_text(json.dumps(entry))
            (out / 'draft-verification.json').write_text('{"ok":true}')
            (out / 'verification-sha256sums.txt').write_text('hash  setup.exe\nhash  test-sdk-index.json\n')
            with patch('sys.argv', ['publish_sdk', 'draft', '--directory', str(out)]), patch.dict(publish_sdk.os.environ, {'GITHUB_SHA': 'commit', 'GITHUB_REF_TYPE': 'branch'}), patch.object(publish_sdk.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)), patch.object(publish_sdk, 'run') as run:
                publish_sdk.main()
            uploads = [call.args for call in run.call_args_list if call.args[:3] == ('gh', 'release', 'upload')]
            public = next(args for args in uploads if args[3] == 'sdk-v1.2.3')
            support = next(args for args in uploads if args[3] == 'sdk-support-v1.2.3')
            self.assertFalse(any('test-sdk-index.json' in str(value) for value in public))
            self.assertTrue(any('#Windows' in str(value) for value in public))
            self.assertIn(out / 'test-sdk-index.json', support)
            self.assertNotIn(out / 'setup.exe', support)


class QuickPublicVerification(unittest.TestCase):
    def test_requires_matching_installed_evidence_and_exact_public_bytes(self):
        import hashlib
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary).resolve()
            entry = {'version': '1.2.3', 'entry': {'url': 'https://example.org/sdk-manifest.json'},
                     'installer': {'name': 'setup.exe'}, 'manager': {'name': 'manager.zip'}}
            (out / 'channel-entry.json').write_text(json.dumps(entry))
            (out / 'draft-verification.json').write_text(json.dumps({'ok': True, 'stage': 'draft', 'sdk_version': '1.2.3'}))
            (out / 'sha256sums.txt').write_text(hashlib.sha256(b'installer').hexdigest() + '  setup.exe\n')
            def download(url, path): path.write_bytes(b'installer')
            argv = ['verify', '--directory', str(out), '--public', '--quick-public']
            with patch('sys.argv', argv), patch.object(verification, 'download_public', side_effect=download):
                verification.main()
            self.assertTrue(json.loads((out / 'public-verification.json').read_text())['ok'])
            (out / 'public-verification.json').unlink()
            with patch('sys.argv', argv), patch.object(verification, 'download_public', side_effect=lambda url, path: path.write_bytes(b'changed')):
                with self.assertRaises(SystemExit): verification.main()
            self.assertFalse((out / 'public-verification.json').exists())
            (out / 'draft-verification.json').write_text('{"ok":false}')
            with patch('sys.argv', argv), patch.object(verification, 'download_public') as request:
                with self.assertRaises(SystemExit): verification.main()
                request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
