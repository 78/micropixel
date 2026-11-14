#!/usr/bin/env python3
"""Run the upstream CLI over Bouffalo UART without toggling boot/reset lines."""
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
import serial

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="backslashreplace")
path = Path(__file__).resolve().parents[3] / "tools/micropixel"
loader = importlib.machinery.SourceFileLoader("micropixel_upstream_cli", str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
upstream = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = upstream
loader.exec_module(upstream)

def open_uart(port):
    device = serial.Serial(None, 115200, timeout=.05, write_timeout=10)
    device.dtr = False
    device.rts = False
    device.port = port
    device.open()
    return device

# The upstream default toggles DTR/RTS for ESP USB Serial/JTAG. This board
# wires those lines to automatic programming, so retain the UART line state.
upstream.open_usb_serial = open_uart

_upstream_aot_target_for_chip = upstream.aot_target_for_chip
def aot_target_for_chip(chip):
    if isinstance(chip, str) and chip.strip().upper() in ("BL616CL", "BL618DG"):
        return "riscv32-ilp32f"
    return _upstream_aot_target_for_chip(chip)

_upstream_require_firmware = upstream.require_compatible_usb_firmware
def require_firmware(status):
    if (status.get("hardware", {}).get("chip") in ("BL616CL", "BL618DG")
            and status.get("firmware", {}).get("version") == "bouffalo-dev"):
        return
    return _upstream_require_firmware(status)

upstream.aot_target_for_chip = aot_target_for_chip
upstream.require_compatible_usb_firmware = require_firmware
if __name__ == "__main__":
    raise SystemExit(upstream.main())
