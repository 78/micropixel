#!/usr/bin/env python3
"""Capture a MicroPixel LVGL screen as hardware-encoded JPEG over USB."""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import serial


BEGIN = b"\nMICROPIXEL_CAPTURE_BEGIN "
COMMAND_PREFIX = "MICROPIXEL_CAPTURE"


def open_serial_without_reset(port: str, timeout: float = 0.05) -> serial.Serial:
    """Open a MicroPixel USB console without generating a reset pulse."""
    device = serial.Serial(port=None, baudrate=115200, timeout=timeout, exclusive=False)
    device.dtr = True
    device.rts = True
    device.port = port
    device.open()
    device.rts = False
    device.dtr = False
    return device


def hard_reset(device: serial.Serial) -> None:
    """Generate idf_monitor's RTS-only application-reset sequence."""
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


def extract_jpeg(buffer: bytearray) -> tuple[bytes, int, int, int, str] | None:
    begin = buffer.find(BEGIN)
    if begin < 0:
        return None
    header_end = buffer.find(b"\n", begin + 1)
    if header_end < 0:
        return None
    fields = bytes(buffer[begin + len(BEGIN) : header_end]).split()
    if len(fields) != 6 or fields[3] != b"JPEG":
        del buffer[: header_end + 1]
        return None
    sequence, width, height = map(int, fields[:3])
    jpeg_size = int(fields[4])
    source = fields[5].decode("ascii")
    jpeg_start = header_end + 1
    jpeg_end = jpeg_start + jpeg_size
    end_marker = f"\nMICROPIXEL_CAPTURE_END {sequence}\n".encode()
    marker_end = jpeg_end + len(end_marker)
    if len(buffer) < marker_end:
        return None
    jpeg = bytes(buffer[jpeg_start:jpeg_end])
    if len(jpeg) < 4 or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise RuntimeError("capture payload is not a complete JPEG stream")
    if buffer[jpeg_end:marker_end] != end_marker:
        raise RuntimeError("capture end marker missing or corrupted")
    del buffer[:marker_end]
    return jpeg, sequence, width, height, source


def request_capture(
    device: serial.Serial, timeout: float, source: str = "LOGICAL"
) -> tuple[bytes, int, int, int]:
    source = source.upper()
    if source not in {"LOGICAL", "DISPLAY"}:
        raise ValueError(f"unsupported capture source: {source}")
    command = f"{COMMAND_PREFIX} {source}\n".encode()
    received = bytearray()
    deadline = time.monotonic() + timeout
    next_request = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if BEGIN not in received and now >= next_request:
            device.write(command)
            next_request = now + 2.0
        data = device.read(16384)
        if data:
            received.extend(data)
            result = extract_jpeg(received)
            if result is not None:
                jpeg, sequence, width, height, captured_source = result
                if captured_source != source:
                    raise RuntimeError(
                        f"capture source mismatch: requested {source}, received {captured_source}"
                    )
                return jpeg, sequence, width, height
            if BEGIN not in received and len(received) > 65536:
                del received[: -len(BEGIN)]
    tail = bytes(received[-4000:]).decode(errors="replace")
    raise TimeoutError(f"timed out waiting for screenshot; serial tail:\n{tail}")


def output_path(
    base: pathlib.Path, index: int, count: int, sequence: int, source: str
) -> pathlib.Path:
    if count == 1 and base.suffix.lower() in {".jpg", ".jpeg"}:
        return base
    base.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    return base / f"micropixel-{timestamp}-{index + 1:02d}-{source.lower()}-{sequence}.jpg"


def parse_size(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", 1)
        width, height = int(width_text), int(height_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("expected positive WIDTHxHEIGHT")
    return width, height


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--source", choices=("logical", "display"), default="logical")
    parser.add_argument("--expect-size", type=parse_size)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--reset", action="store_true", help="reset after opening, preserving boot logs")
    parser.add_argument("--wait-for", help="wait for this serial log text before the first capture")
    parser.add_argument("--post-trigger-delay", type=float, default=0.0)
    parser.add_argument("--interval", type=float, default=0.25)
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least one")

    source = args.source.upper()
    with open_serial_without_reset(args.port) as device:
        if args.reset:
            hard_reset(device)
        if args.wait_for:
            wait_for_log(device, args.wait_for.encode(), args.timeout)
            if args.post_trigger_delay > 0.0:
                time.sleep(args.post_trigger_delay)
        for index in range(args.count):
            jpeg, sequence, width, height = request_capture(device, args.timeout, source)
            if args.expect_size is not None and (width, height) != args.expect_size:
                raise RuntimeError(
                    f"unexpected screenshot size {width}x{height}; expected "
                    f"{args.expect_size[0]}x{args.expect_size[1]}"
                )
            path = output_path(args.output, index, args.count, sequence, source)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(jpeg)
            print(
                f"captured {width}x{height} {source.lower()} JPEG #{sequence}: "
                f"{path} ({len(jpeg)} bytes)"
            )
            if index + 1 < args.count:
                time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, serial.SerialException, RuntimeError, TimeoutError) as error:
        print(f"capture failed: {error}", file=sys.stderr)
        raise SystemExit(1)
