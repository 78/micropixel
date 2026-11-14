# SPDX-License-Identifier: Apache-2.0
"""Exercise target selection without opening a physical UART."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bouffalo_control', ROOT/'tools/control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)

class ControlCompatibility(unittest.TestCase):
    def test_both_bouffalo_targets(self):
        for chip in ('BL616CL', 'BL618DG'):
            self.assertEqual(control.aot_target_for_chip(chip), 'riscv32-ilp32f')
            control.require_firmware({'hardware': {'chip': chip}, 'firmware': {'version': 'bouffalo-dev'}})
    def test_existing_targets_keep_their_abi(self):
        for chip, target in [('ESP32-S3', 'xtensa'), ('ESP32-P4', 'riscv32-ilp32f'), ('ESP32-S31', 'riscv32-ilp32f')]:
            self.assertEqual(control.aot_target_for_chip(chip), target)
    def test_unknown_chips_and_old_firmware_are_rejected(self):
        with self.assertRaises(control.upstream.CliError):
            control.aot_target_for_chip('unsupported')
        with self.assertRaises(control.upstream.CompatibilityFailure):
            control.require_firmware({'hardware': {'chip': 'ESP32-P4'}, 'firmware': {'version': '0.0.0'}})
        with self.assertRaises(control.upstream.CompatibilityFailure):
            control.require_firmware({'hardware': {'chip': 'BL618DG'}, 'firmware': {'version': '0.0.0'}})
