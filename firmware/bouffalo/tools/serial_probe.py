#!/usr/bin/env python3
"""Capture boot and query the MicroPixel control protocol (requires pyserial)."""
import argparse
from pathlib import Path
import re
import time
import serial
import sys
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="backslashreplace")

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, default=20)
parser.add_argument("--log", type=Path, default=Path(__file__).resolve().parents[1] / "dist/serial.log")
parser.add_argument("--command", action="append")
parser.add_argument("--capture", type=Path)
parser.add_argument("--reset", action="store_true",
                    help="Reset into Flash boot using the SDK DTR/RTS sequence")
args = parser.parse_args()
commands = args.command or ["MPX1 1 HELLO", "MPX1 2 DEVICE_STATUS", "MPX1 3 APP_LAST_ERROR"]
port = serial.Serial(None, 115200, timeout=.1)
# Keep the board out of reset and UART download mode.
port.dtr = False
port.rts = False
port.port = args.port
data = bytearray()
with port:
    if args.reset:
        # Match tools/autotest/tests/libs/lib_serial.py serial_auto_open:
        # select Flash boot with DTR while RTS holds reset, then release RTS.
        port.rts = True
        time.sleep(.05)
        port.dtr = False
        time.sleep(.05)
        port.dtr = True
        time.sleep(.05)
        port.rts = False
    start = time.monotonic()
    next_query = start + 1
    requested_capture = False
    while time.monotonic() - start < args.seconds:
        now = time.monotonic()
        if now >= next_query:
            for command in commands:
                port.write(command.encode("ascii") + b"\n")
            next_query = now + 3
        if args.capture and not requested_capture and now - start > 4:
            port.write(b"MICROPIXEL_CAPTURE\n")
            requested_capture = True
        data.extend(port.read(8192))
args.log.parent.mkdir(parents=True, exist_ok=True)
args.log.write_bytes(data)
if args.capture:
    match = re.search(rb"MICROPIXEL_CAPTURE_BEGIN (\d+) (\d+) (\d+) JPEG (\d+) DISPLAY\r?\n", data)
    if match:
        size = int(match[4])
        image = data[match.end():match.end() + size]
        if len(image) == size and image.startswith(b"\xff\xd8") and image.endswith(b"\xff\xd9"):
            args.capture.parent.mkdir(parents=True, exist_ok=True)
            args.capture.write_bytes(image)
            print(f"Screenshot: {args.capture} ({match[2].decode()} x {match[3].decode()})")
        else:
            raise SystemExit("Incomplete or corrupted screenshot")
    else:
        raise SystemExit("No screenshot frame received")
# Keep exception dumps in the raw log and show just the useful prefix/replies.
printable = bytes(data)
# Exclude length-delimited JPEG payloads from terminal output, retain raw log.
for frame in reversed(list(re.finditer(rb"MICROPIXEL_CAPTURE_BEGIN \d+ \d+ \d+ JPEG (\d+) DISPLAY\r?\n", printable))):
    start = frame.end()
    printable = printable[:start] + b"[JPEG payload omitted]" + printable[start + int(frame[1]):]
text = printable.decode("utf-8", errors="replace").replace("\x00", "")
prefix = text.split("------ DATA BEGIN", 1)[0]
print(prefix[:16000])
for line in text[len(prefix):].splitlines():
    if line.startswith("MPX1 "):
        print(line)
print(f"Captured {len(data)} bytes to {args.log}")
