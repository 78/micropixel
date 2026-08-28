#!/usr/bin/env python3
"""Inject touch gestures into a screen-capture Host and optionally capture the result."""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

import serial

from capture_p4_screen import (
    hard_reset,
    open_serial_without_reset,
    request_capture,
    wait_for_log,
)


def coordinate(value: str) -> int:
    parsed = int(value)
    if not 0 <= parsed < 720:
        raise ValueError(f"touch coordinate is outside the 720x720 display: {parsed}")
    return parsed


def send_touch(
    device: serial.Serial,
    phase: str,
    touch_id: int,
    x: int,
    y: int,
    pressure: int,
) -> None:
    # Use an explicit terminal line ending. USB CDC transports are allowed to
    # preserve packet boundaries differently, so LF-only writes can otherwise
    # remain buffered and concatenate several injected samples on Mosaico.
    command = f"MICROPIXEL_TOUCH {phase} {touch_id} {x} {y} {pressure}\r\n".encode()
    device.write(command)
    time.sleep(0.025)


def parse_gesture(specification: str) -> tuple[str, list[int]]:
    fields = specification.split(":")
    kind = fields[0].lower()
    expected = {"tap": 3, "flick": 6, "swipe": 7}
    if kind not in expected or len(fields) != expected[kind]:
        forms = "tap:x:y, flick:x1:y1:x2:y2:duration_ms, swipe:x1:y1:x2:y2:duration_ms:steps"
        raise ValueError(f"invalid gesture {specification!r}; expected one of: {forms}")
    values = [int(field) for field in fields[1:]]
    return kind, values


def play_gesture(device: serial.Serial, touch_id: int, specification: str) -> None:
    kind, values = parse_gesture(specification)
    if kind == "tap":
        x, y = map(lambda value: coordinate(str(value)), values)
        send_touch(device, "DOWN", touch_id, x, y, 1)
        time.sleep(0.035)
        send_touch(device, "UP", touch_id, x, y, 0)
        return

    x1, y1, x2, y2 = map(lambda value: coordinate(str(value)), values[:4])
    duration_ms = values[4]
    if not 1 <= duration_ms <= 5000:
        raise ValueError("gesture duration must be between 1 and 5000 ms")
    send_touch(device, "DOWN", touch_id, x1, y1, 1)

    if kind == "swipe":
        steps = values[5]
        if not 1 <= steps <= 100:
            raise ValueError("swipe steps must be between 1 and 100")
        step_delay = duration_ms / steps / 1000.0
        for step in range(1, steps + 1):
            time.sleep(step_delay)
            x = x1 + (x2 - x1) * step // steps
            y = y1 + (y2 - y1) * step // steps
            send_touch(device, "MOVE", touch_id, x, y, 1)
    else:
        time.sleep(duration_ms / 1000.0)

    send_touch(device, "UP", touch_id, x2, y2, 0)


def drain_logs(device: serial.Serial, quiet_seconds: float = 0.15) -> None:
    deadline = time.monotonic() + quiet_seconds
    while time.monotonic() < deadline:
        data = device.read(4096)
        if not data:
            continue
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        deadline = time.monotonic() + quiet_seconds


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument(
        "--gesture",
        action="append",
        default=[],
        help="repeatable tap:x:y, flick:x1:y1:x2:y2:ms, or swipe:x1:y1:x2:y2:ms:steps",
    )
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--wait-for", default="blocks: ready")
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--between", type=float, default=0.12, help="seconds between gestures")
    parser.add_argument("--settle", type=float, default=0.5, help="seconds before the optional capture")
    parser.add_argument("--show-logs", action="store_true", help="print serial logs emitted by the injected events")
    args = parser.parse_args()

    if not args.gesture:
        parser.error("at least one --gesture is required")

    with open_serial_without_reset(args.port) as device:
        if args.reset:
            hard_reset(device)
        if args.wait_for:
            wait_for_log(device, args.wait_for.encode(), args.timeout)
        for index, specification in enumerate(args.gesture):
            play_gesture(device, index + 1, specification)
            if index + 1 < len(args.gesture):
                time.sleep(args.between)
        time.sleep(args.settle)

        if args.show_logs:
            drain_logs(device)

        if args.output is not None:
            jpeg, sequence, width, height = request_capture(device, args.timeout)
            if (width, height) != (720, 720):
                raise RuntimeError(f"unexpected screenshot size {width}x{height}")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(jpeg)
            print(f"captured {width}x{height} JPEG #{sequence}: {args.output} ({len(jpeg)} bytes)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, serial.SerialException, RuntimeError, TimeoutError) as error:
        print(f"touch drive failed: {error}")
        raise SystemExit(1)
