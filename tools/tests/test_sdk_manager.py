import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools.manager import micropixel_manager as m


class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manager = m.Manager(self.root / 'cache')

    def manifest(self, version):
        value = {'schema_version': 1, 'sdk_version': version, 'toolchain_id': 'tools-1'}
        data = json.dumps(value).encode()
        path = self.manager.root / 'manifests' / (version + '.json')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return value, m.digest(data)

    def test_switch_failure_preserves_lock_and_other_project(self):
        self.manifest('1.0.0')
        self.manifest('2.0.0')
        a, b = self.root / 'a', self.root / 'b'
        with patch.object(self.manager, 'prepare', return_value={}):
            self.manager.switch(a, '1.0.0')
            self.manager.switch(b, '1.0.0')
        before = (a / m.LOCK_NAME).read_bytes()
        with patch.object(self.manager, 'prepare', side_effect=m.Failure('download_failed', 'offline')):
            with self.assertRaises(m.Failure):
                self.manager.switch(a, '2.0.0')
        self.assertEqual((a / m.LOCK_NAME).read_bytes(), before)
        with patch.object(self.manager, 'prepare', return_value={}):
            self.manager.switch(a, '2.0.0')
        self.assertEqual(m.read_json(b / m.LOCK_NAME)['sdk_version'], '1.0.0')
        self.manager.offline = True
        with patch.object(self.manager, 'prepare', return_value={}):
            self.manager.switch(a, '1.0.0')
        self.assertEqual((a / m.LOCK_NAME).read_bytes(), before)

    def test_unknown_lock_never_overwritten(self):
        project = self.root / 'project'
        m.atomic_json(project / m.LOCK_NAME, {'schema_version': 2})
        with self.assertRaises(m.Failure) as raised:
            self.manager.switch(project, '2.0.0')
        self.assertEqual(raised.exception.code, 'incompatible_lock')
        self.assertEqual(m.read_json(project / m.LOCK_NAME), {'schema_version': 2})

    def test_manifest_digest_is_enforced_on_cached_data(self):
        self.manifest('1.0.0')
        with self.assertRaises(m.Failure) as raised:
            self.manager.manifest('1.0.0', expected='0' * 64)
        self.assertEqual(raised.exception.code, 'checksum_mismatch')

    def test_offline_and_network_failure_keep_update_state_honest(self):
        index = {'schema_version': 1, 'stable': '2.0.0', 'versions': {'2.0.0': {'release_notes_url': 'https://example.org/notes'}}}
        with patch.object(m, 'fetch', return_value=json.dumps(index).encode()) as request:
            state = self.manager.status('1.0.0', force=True)
            self.assertEqual(state['check_status'], 'checked')
            self.manager.status('1.0.0')
            self.assertEqual(request.call_count, 1)
        self.manager.offline = True
        with patch.object(m, 'fetch', side_effect=AssertionError('must not connect')):
            self.assertEqual(self.manager.status('1.0.0')['check_status'], 'offline')
        self.manager.offline = False
        with patch.object(m, 'fetch', side_effect=OSError('network down')):
            self.assertEqual(self.manager.status('1.0.0', force=True)['check_status'], 'unavailable')
        self.assertTrue(any(w['code'] == 'sdk_update_available' for w in self.manager.warnings))

    def test_install_lock_excludes_second_writer(self):
        with m.install_lock(self.manager.root):
            with self.assertRaises(m.Failure) as raised:
                with m.install_lock(self.manager.root):
                    pass
            self.assertEqual(raised.exception.code, 'installation_busy')
        with m.install_lock(self.manager.root):
            pass

    def test_archive_rejects_traversal_windows_aliases_and_links(self):
        for name in ('../escape', '/escape', 'C:/escape', 'a\\b', 'safe/file:stream', 'CON.txt', 'foo./x'):
            with self.subTest(name=name), self.assertRaises(m.Failure):
                m.safe_path(name)
        archive = self.root / 'evil.zip'
        with zipfile.ZipFile(archive, 'w') as output:
            info = zipfile.ZipInfo('link')
            info.external_attr = 0o120777 << 16
            output.writestr(info, '/etc/passwd')
        with self.assertRaises(m.Failure):
            m.extract(archive, self.root / 'output')

    def test_safe_archive_installs_once_and_reuses_offline(self):
        content = io.BytesIO()
        with zipfile.ZipFile(content, 'w') as output:
            output.writestr('package/tool.exe', b'fixture')
        data = content.getvalue()
        sha = hashlib.sha256(data).hexdigest()
        archive = self.manager.root / 'downloads' / sha
        archive.parent.mkdir(parents=True)
        archive.write_bytes(data)
        spec = {'sha256': sha, 'size_bytes': len(data), 'root': 'package', 'url': 'https://example.org/archive'}
        with m.install_lock(self.manager.root):
            location = self.manager.asset(spec, install=True)
        self.assertEqual((location / 'tool.exe').read_bytes(), b'fixture')
        self.manager.offline = True
        self.assertEqual(self.manager.asset(spec), location)

    def test_manager_json_init_dispatch_and_lock_end_to_end(self):
        from tools.build_sdk_release import build
        repository = Path(__file__).resolve().parents[2]
        metadata, data = build(repository)
        version = metadata['version']
        root = self.manager.root
        sha = m.digest(data)
        archive = root / 'downloads' / sha
        archive.parent.mkdir(parents=True)
        archive.write_bytes(data)
        sdk = {'sha256': sha, 'size_bytes': len(data), 'root': 'micropixel-sdk-' + version}
        with m.install_lock(root):
            self.manager.asset(sdk, install=True)
        tools_sha = '1' * 64
        tool_root = root / 'packages' / tools_sha
        (tool_root / 'tools/bin').mkdir(parents=True)
        (tool_root / 'tools/bin/clang++.exe').touch()
        (tool_root / 'tools/wamrc.exe').touch()
        m.atomic_json(tool_root / '.complete.json', {'sha256': tools_sha})
        tool = {'sha256': tools_sha, 'root': 'tools'}
        manifest = {'schema_version': 1, 'sdk_version': version, 'toolchain_id': 'fixture-tools',
                    'sdk': sdk, 'platforms': {'windows-x64': {'wasi': tool, 'wamrc_riscv': tool, 'wamrc_xtensa': tool}}}
        path = root / 'manifests' / (version + '.json')
        m.atomic_json(path, manifest)
        m.atomic_json(root / 'default.json', {'sdk_version': version, 'manifest_sha256': m.file_digest(path)})
        m.atomic_json(root / 'index-cache.json', {'checked_at': 1, 'index': {'schema_version': 1, 'stable': '99.0.0', 'versions': {'99.0.0': {}}}})
        project = self.root / '中文 game'
        env = {**os.environ, 'MICROPIXEL_HOME': str(root)}
        command = [sys.executable, str(repository / 'tools/manager/micropixel_manager.py')]
        result = subprocess.run([*command, 'init', str(project), '--app-id', 'test.manager', '--title', 'Manager', '--offline', '--json'], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        value = json.loads(result.stdout)
        self.assertTrue(value['ok'])
        self.assertEqual(m.read_json(project / m.LOCK_NAME)['sdk_version'], version)
        result = subprocess.run([*command, 'sdk', 'status', '--project', str(project), '--offline', '--json'], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value['result']['check_status'], 'offline')
        self.assertEqual(value['warnings'][0]['code'], 'sdk_update_available')

    def test_older_stable_is_not_reported_as_upgrade(self):
        self.assertFalse(m.newer('1.0.0', '2.0.0'))
        self.assertTrue(m.newer('1.10.0', '1.9.0'))
        self.assertTrue(m.newer('1.0.0', '1.0.0-preview.1'))


    def test_interrupted_download_cleans_partial_and_retry_succeeds(self):
        content = io.BytesIO()
        with zipfile.ZipFile(content, 'w') as output:
            output.writestr('package/tool.exe', b'fixture')
        data = content.getvalue()
        sha = m.digest(data)
        spec = {'sha256': sha, 'size_bytes': len(data), 'root': 'package', 'url': 'https://example.org/archive'}
        class Response(io.BytesIO):
            url = spec['url']
        class Broken(Response):
            def read(self, size=-1):
                raise OSError('connection interrupted')
        with patch.object(m.urllib.request, 'urlopen', return_value=Broken(data)):
            with self.assertRaises(OSError):
                self.manager.asset(spec, install=True)
        self.assertFalse((self.manager.root / 'downloads' / (sha + '.part')).exists())
        self.assertFalse((self.manager.root / 'packages' / sha).exists())
        with patch.object(m.urllib.request, 'urlopen', return_value=Response(data)):
            self.assertTrue((self.manager.asset(spec, install=True) / 'tool.exe').exists())


    def test_app_local_crt_does_not_mutate_original_wasi_cache(self):
        wasi, crt = self.root / 'wasi', self.root / 'crt'
        (wasi / 'bin').mkdir(parents=True)
        (wasi / 'bin/clang++.exe').write_bytes(b'compiler')
        (wasi / 'bin/msvcp140.dll').write_bytes(b'original')
        crt.mkdir()
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            (crt / name).write_bytes(b'redistributed')
        platform = {'wasi': {'sha256': 'a' * 64}, 'msvc_crt': {'sha256': 'b' * 64}}
        prepared = self.manager.compose_wasi(wasi, crt, platform, True)
        self.assertEqual((prepared / 'bin/msvcp140.dll').read_bytes(), b'redistributed')
        self.assertEqual((wasi / 'bin/msvcp140.dll').read_bytes(), b'original')
        self.assertEqual(self.manager.compose_wasi(wasi, crt, platform, False), prepared)



if __name__ == '__main__':
    unittest.main()
