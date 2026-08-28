#!/usr/bin/env python3
"""Compatibility entry point for the board-neutral USB JPEG capture tool."""

from capture_screen import (  # noqa: F401
    hard_reset,
    main,
    open_serial_without_reset,
    request_capture,
    wait_for_log,
)


if __name__ == "__main__":
    raise SystemExit(main())
