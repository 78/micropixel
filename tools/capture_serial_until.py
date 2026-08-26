#!/usr/bin/env python3
"""Capture an ESP-IDF serial log until an expected summary is observed."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

import serial

from capture_p4_screen import open_serial_without_reset


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("expected")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--output",
        type=Path,
        help="also save the complete captured byte stream to this file",
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="hard-reset the target after opening the serial port",
    )
    args = parser.parse_args()

    output = None
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        output = args.output.open("wb")

    expected = args.expected.encode()
    observed = bytearray()
    deadline = time.monotonic() + args.timeout
    reset_pending = args.reset
    while time.monotonic() < deadline:
        try:
            with open_serial_without_reset(args.port) as device:
                if reset_pending:
                    # Match esptool's USB Serial/JTAG hard-reset sequence while the
                    # capture endpoint is already open, so early startup logs are
                    # not lost between reset and reopening the port.
                    device.dtr = False
                    device.rts = True
                    time.sleep(0.1)
                    device.rts = False
                    time.sleep(0.1)
                    reset_pending = False
                while time.monotonic() < deadline:
                    data = device.read(4096)
                    if not data:
                        continue
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
                    if output is not None:
                        output.write(data)
                        output.flush()
                    observed.extend(data)
                    if expected in observed:
                        # The match can arrive before the remainder of the same
                        # log line. Drain one short serial interval so saved
                        # evidence contains the complete summary record.
                        time.sleep(0.1)
                        trailing = device.read(4096)
                        if trailing:
                            sys.stdout.buffer.write(trailing)
                            sys.stdout.buffer.flush()
                            if output is not None:
                                output.write(trailing)
                                output.flush()
                        if output is not None:
                            output.close()
                        return 0
                    if len(observed) > 32768:
                        del observed[:-16384]
        except (OSError, serial.SerialException) as error:
            # USB Serial/JTAG can briefly re-enumerate on macOS without the
            # target resetting. Keep the same deadline and never reset again.
            print(f"Serial disconnected; retrying: {error}", file=sys.stderr)
            time.sleep(0.25)

    print(
        f"\nTimed out waiting for serial summary: {args.expected}",
        file=sys.stderr,
    )
    if output is not None:
        output.close()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
