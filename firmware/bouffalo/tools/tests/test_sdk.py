# SPDX-License-Identifier: Apache-2.0
"""Exercise SDK provenance checks and non-destructive preparation with local Git."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdkPreparation(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.platform = self.root/'platform'
        (self.platform/'tools').mkdir(parents=True)
        for name in ('prepare_sdk.py', 'check_sdk.py'):
            shutil.copy2(ROOT/'tools'/name, self.platform/'tools'/name)
        self.sdk = self.root/'source'
        self.git('init', str(self.sdk), cwd=self.root)
        required = {
            'bsp/board/bl616cldk/board_gpio.c': 'board_gpio_overlay.h',
            'bsp/board/bl616cldk/bl616cl_common.ld.in': '__tls_size',
            'bsp/board/bl618dgdk/bl618dg_common.ld.in': '__tls_size',
            'bsp/common/audio_player/wave_player.h':
                'required_ext_codec_type playback_task_priority wave_player_set_mute',
            'bsp/common/audio_codec/audio_codec.h': 'AUDIO_CODEC_TYPE_ES8388 AUDIO_CODEC_TYPE_JY6311',
            'components/os/freertos/include/portable.h': 'configSTACK_ALLOCATION_FROM_SEPARATE_HEAP',
            'bsp/Kconfig': 'config BSP_SDH_TIMEOUT_MS',
            'critical.c': 'original\n',
        }
        for name, text in required.items():
            path = self.sdk/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.git('add', '.')
        self.git('-c', 'user.name=SDK Test', '-c', 'user.email=test@example.invalid',
                 'commit', '-m', 'SDK baseline')
        revision = self.git('rev-parse', 'HEAD').stdout.strip()
        patch = self.platform/'sdk.patch'
        patch.write_text('diff --git a/critical.c b/critical.c\n'
                         '--- a/critical.c\n+++ b/critical.c\n@@ -1 +1 @@\n-original\n+patched\n')
        self.source = {'repository': self.sdk.as_uri(), 'commit': revision, 'patches': [{
            'path': 'sdk.patch', 'sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
            'files': ['critical.c'],
        }]}
        (self.platform/'sdk-source.json').write_text(json.dumps(self.source))
        (self.platform/'sdk-baseline.json').write_text(json.dumps({
            'sha256': {'critical.c': hashlib.sha256(b'patched\n').hexdigest()},
        }))
        self.target = self.root/'prepared'

    def git(self, *args, cwd=None):
        return subprocess.run(['git', *args], cwd=cwd or self.sdk,
                              capture_output=True, text=True, check=True)

    def tool(self, script, *args):
        return subprocess.run([sys.executable, str(self.platform/'tools'/script), *args],
                              capture_output=True, text=True)

    def prepare(self):
        result = self.tool('prepare_sdk.py', '--directory', str(self.target))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_prepare_applies_patch_and_repeated_run_preserves_checkout(self):
        self.prepare()
        self.assertEqual((self.target/'critical.c').read_text(), 'patched\n')
        result = self.tool('check_sdk.py', '--sdk', str(self.target), '--exact')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.prepare()
        result = self.tool('prepare_sdk.py', '--directory', str(self.sdk))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.sdk/'critical.c').read_text(), 'original\n')

    def test_cmake_loads_verified_sdk_despite_competing_search_paths(self):
        cmake = shutil.which('cmake')
        if not cmake:
            self.skipTest('CMake is required for the SDK selection regression')
        (self.platform/'cmake').mkdir()
        shutil.copy2(ROOT/'cmake/sdk.cmake', self.platform/'cmake/sdk.cmake')
        config = self.sdk/'cmake/bouffalo_sdk-config.cmake'
        config.parent.mkdir()
        config.write_text('set_property(GLOBAL PROPERTY SDK_LIBS "")\n'
                          'function(sdk_add_compile_options)\nendfunction()\n'
                          'function(sdk_add_link_options)\nendfunction()\n'
                          'file(WRITE "${CMAKE_BINARY_DIR}/selected-sdk" "${CMAKE_CURRENT_LIST_DIR}")\n')
        baseline = json.loads((self.platform/'sdk-baseline.json').read_text())
        baseline['sha256']['cmake/bouffalo_sdk-config.cmake'] = hashlib.sha256(config.read_bytes()).hexdigest()
        (self.platform/'sdk-baseline.json').write_text(json.dumps(baseline))
        self.git('add', '.')
        self.git('-c', 'user.name=SDK Test', '-c', 'user.email=test@example.invalid',
                 'commit', '-m', 'SDK configuration')
        self.source['commit'] = self.git('rev-parse', 'HEAD').stdout.strip()
        (self.platform/'sdk-source.json').write_text(json.dumps(self.source))
        self.prepare()
        other = self.root/'competing-sdk'
        other.mkdir()
        (other/'bouffalo_sdk-config.cmake').write_text(
            'message(FATAL_ERROR "Loaded SDK that was not verified")\n')
        project = self.root/'project'
        project.mkdir()
        (project/'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(selection NONE)\n'
            f'include("{self.platform}/cmake/sdk.cmake")\n')
        output = self.root/'build'
        env = dict(os.environ, BL_SDK_BASE=str(self.target), CMAKE_PREFIX_PATH=str(other))
        result = subprocess.run([cmake, '-S', str(project), '-B', str(output),
                                 '-Dbouffalo_sdk_DIR='+str(other)],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((output/'selected-sdk').read_text(), str(self.target/'cmake'))

    def test_rejects_wrong_revision_and_undeclared_changes(self):
        self.prepare()
        self.git('-C', str(self.target), '-c', 'user.name=SDK Test',
                 '-c', 'user.email=test@example.invalid', 'commit', '--allow-empty', '-m', 'different SDK')
        result = self.tool('check_sdk.py', '--sdk', str(self.target), '--exact')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Git HEAD', result.stderr)
        self.git('-C', str(self.target), 'checkout', '--detach', self.source['commit'])
        (self.target/'bsp/Kconfig').write_text('config BSP_SDH_TIMEOUT_MS\n# unexpected\n')
        result = self.tool('check_sdk.py', '--sdk', str(self.target), '--exact')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('undeclared SDK changes', result.stderr)

    def test_rejects_modified_patched_file(self):
        self.prepare()
        (self.target/'critical.c').write_text('unexpected\n')
        result = self.tool('check_sdk.py', '--sdk', str(self.target), '--exact')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('critical.c', result.stderr)

    def test_rejects_modified_patch_before_creating_destination(self):
        (self.platform/'sdk.patch').write_text('damaged patch\n')
        result = self.tool('prepare_sdk.py', '--directory', str(self.target))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.target.exists())

    def test_rejects_untracked_sdk_source(self):
        self.prepare()
        (self.target/'extra.c').write_text('untracked source\n')
        result = self.tool('check_sdk.py', '--sdk', str(self.target), '--exact')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('untracked SDK files', result.stderr)


if __name__ == '__main__':
    unittest.main()
