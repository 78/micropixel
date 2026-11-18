# SPDX-License-Identifier: Apache-2.0
"""Ensure Bouffalo-only changes do not schedule unrelated ESP builds."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[4]/'tools/ci'))
from plan_pr_builds import MATRIX, select


class PrBuildSelection(unittest.TestCase):
    def test_bouffalo_sources_do_not_select_espressif(self):
        self.assertEqual(select(['firmware/bouffalo/common/memory.c',
                                 'firmware/bouffalo/boards/bl616cl/defconfig']), [])

    def test_mixed_change_keeps_selected_esp_board(self):
        selected = select(['firmware/bouffalo/cmake/sdk.cmake',
                           'firmware/espressif/main/platform/boards/esp-mosaico/board.cpp'])
        self.assertEqual([entry['profile'] for entry in selected], ['esp-mosaico'])

    def test_shared_firmware_and_abi_still_select_all_esp_boards(self):
        for path in ('firmware/espressif/main/runtime/abi/core.cpp',
                     'guest/abi/micropixel_abi.h', 'tools/ci/plan_pr_builds.py'):
            with self.subTest(path=path):
                self.assertEqual(select([path]), MATRIX)
