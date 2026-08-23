#!/usr/bin/env python3
"""Request and verify one or more MicroPixel LVGL screenshots over USB Serial/JTAG."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import time
import zlib

import serial


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
BEGIN = b"\nMICROPIXEL_CAPTURE_BEGIN "
COMMAND = b"MICROPIXEL_CAPTURE\n"


def hard_reset(device: serial.Serial) -> None:
    """Reset after opening so boot logs and deterministic trigger are not lost."""
    device.dtr = False
    device.rts = True
    time.sleep(0.1)
    device.rts = False
    time.sleep(0.1)


def wait_for_log(device: serial.Serial, marker: bytes, timeout: float) -> None:
    observed = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = device.read(4096)
        if not data:
            continue
        observed.extend(data)
        if marker in observed:
            return
        if len(observed) > 65536:
            del observed[:-32768]
    tail = bytes(observed[-4000:]).decode(errors="replace")
    raise TimeoutError(f"timed out waiting for log marker {marker!r}; serial tail:\n{tail}")


def extract_png(buffer: bytearray) -> tuple[bytes, int, int, int] | None:
    begin = buffer.find(BEGIN)
    if begin < 0:
        return None
    header_end = buffer.find(b"\n", begin + 1)
    if header_end < 0:
        return None
    fields = bytes(buffer[begin + len(BEGIN):header_end]).split()
    if len(fields) != 3:
        del buffer[:header_end + 1]
        return None
    sequence, width, height = map(int, fields)
    png_start = header_end + 1
    if len(buffer) < png_start + len(PNG_SIGNATURE):
        return None
    if buffer[png_start:png_start + len(PNG_SIGNATURE)] != PNG_SIGNATURE:
        del buffer[:png_start]
        return None

    cursor = png_start + len(PNG_SIGNATURE)
    while True:
        if len(buffer) < cursor + 12:
            return None
        chunk_size = struct.unpack(">I", buffer[cursor:cursor + 4])[0]
        chunk_end = cursor + 12 + chunk_size
        if len(buffer) < chunk_end:
            return None
        chunk_type = bytes(buffer[cursor + 4:cursor + 8])
        chunk_data = bytes(buffer[cursor + 8:cursor + 8 + chunk_size])
        expected_crc = struct.unpack(">I", buffer[cursor + 8 + chunk_size:chunk_end])[0]
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(chunk_data, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise RuntimeError(
                f"PNG CRC mismatch in {chunk_type!r}: {actual_crc:08x} != {expected_crc:08x}"
            )
        cursor = chunk_end
        if chunk_type == b"IEND":
            png = bytes(buffer[png_start:cursor])
            end_marker = f"\nMICROPIXEL_CAPTURE_END {sequence}\n".encode()
            marker_end = cursor + len(end_marker)
            if len(buffer) < marker_end:
                return None
            if buffer[cursor:marker_end] != end_marker:
                raise RuntimeError("capture end marker missing or corrupted")
            del buffer[:marker_end]
            return png, sequence, width, height


def request_capture(device: serial.Serial, timeout: float) -> tuple[bytes, int, int, int]:
    received = bytearray()
    deadline = time.monotonic() + timeout
    next_request = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        # Opening an ESP USB Serial/JTAG endpoint can reset the target on some
        # hosts. Retry only until the BEGIN marker is seen so a command sent
        # during early boot is not silently lost and no second capture queues.
        if BEGIN not in received and now >= next_request:
            device.write(COMMAND)
            # USB Serial/JTAG can leave tcdrain() blocked indefinitely on macOS
            # even though write() already handed the short trigger to the tty.
            # The read loop provides the required ordering without a drain.
            next_request = now + 2.0
        data = device.read(16384)
        if data:
            received.extend(data)
            result = extract_png(received)
            if result is not None:
                return result
            if BEGIN not in received and len(received) > 65536:
                del received[:-len(BEGIN)]
    tail = bytes(received[-4000:]).decode(errors="replace")
    raise TimeoutError(f"timed out waiting for screenshot; serial tail:\n{tail}")


def output_path(base: pathlib.Path, index: int, count: int, sequence: int) -> pathlib.Path:
    if count == 1 and base.suffix.lower() == ".png":
        return base
    base.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    return base / f"micropixel-{timestamp}-{index + 1:02d}-board-{sequence}.png"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--reset", action="store_true",
        help="hard-reset after opening the port",
    )
    parser.add_argument(
        "--wait-for",
        help="wait for this serial log text before taking the first screenshot",
    )
    parser.add_argument(
        "--post-trigger-delay", type=float, default=0.0,
        help="seconds to wait after --wait-for is observed",
    )
    parser.add_argument(
        "--interval", type=float, default=0.25,
        help="seconds between requests when --count is greater than one",
    )
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least one")

    with serial.Serial(args.port, 115200, timeout=0.05, exclusive=False) as device:
        if args.reset:
            hard_reset(device)
        if args.wait_for:
            wait_for_log(device, args.wait_for.encode(), args.timeout)
            if args.post_trigger_delay > 0.0:
                time.sleep(args.post_trigger_delay)
        for index in range(args.count):
            png, sequence, width, height = request_capture(device, args.timeout)
            if (width, height) != (720, 720):
                raise RuntimeError(f"unexpected screenshot size {width}x{height}")
            path = output_path(args.output, index, args.count, sequence)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(png)
            print(f"captured {width}x{height} PNG #{sequence}: {path} ({len(png)} bytes)")
            if index + 1 < args.count:
                time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, serial.SerialException, RuntimeError, TimeoutError) as error:
        print(f"capture failed: {error}", file=sys.stderr)
        raise SystemExit(1)
