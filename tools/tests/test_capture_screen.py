#!/usr/bin/env python3
"""Tests for the binary-safe MicroPixel USB JPEG capture framing."""

from __future__ import annotations

import pathlib
import sys
import unittest


WORKSPACE_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(WORKSPACE_ROOT / "tools"))

import capture_screen  # noqa: E402


class CaptureScreenTest(unittest.TestCase):
    def test_extracts_jpeg_surrounded_by_console_noise(self) -> None:
        jpeg = b"\xff\xd8binary\x00\nMICROPIXEL_CAPTURE_END 999\n\xff\xd9"
        buffer = bytearray(
            b"I (123) boot: ready"
            b"\nMICROPIXEL_CAPTURE_BEGIN 7 480 480 JPEG "
            + str(len(jpeg)).encode()
            + b" DISPLAY\n"
            + jpeg
            + b"\nMICROPIXEL_CAPTURE_END 7\nI (456) capture complete\n"
        )

        result = capture_screen.extract_jpeg(buffer)

        self.assertEqual(result, (jpeg, 7, 480, 480, "DISPLAY"))
        self.assertEqual(buffer, b"I (456) capture complete\n")

    def test_waits_for_complete_binary_payload(self) -> None:
        jpeg = b"\xff\xd8payload\xff\xd9"
        buffer = bytearray(
            b"\nMICROPIXEL_CAPTURE_BEGIN 2 720 720 JPEG "
            + str(len(jpeg)).encode()
            + b" LOGICAL\n"
            + jpeg[:-1]
        )

        self.assertIsNone(capture_screen.extract_jpeg(buffer))

        buffer.extend(jpeg[-1:] + b"\nMICROPIXEL_CAPTURE_END 2\n")
        self.assertEqual(capture_screen.extract_jpeg(buffer), (jpeg, 2, 720, 720, "LOGICAL"))

    def test_rejects_corrupted_end_marker(self) -> None:
        jpeg = b"\xff\xd8payload\xff\xd9"
        buffer = bytearray(
            b"\nMICROPIXEL_CAPTURE_BEGIN 3 480 480 JPEG "
            + str(len(jpeg)).encode()
            + b" LOGICAL\n"
            + jpeg
            + b"\nMICROPIXEL_CAPTURE_END 4\n"
        )

        with self.assertRaisesRegex(RuntimeError, "end marker"):
            capture_screen.extract_jpeg(buffer)

    def test_parse_size_validates_positive_dimensions(self) -> None:
        self.assertEqual(capture_screen.parse_size("480x480"), (480, 480))
        with self.assertRaises(Exception):
            capture_screen.parse_size("0x480")


if __name__ == "__main__":
    unittest.main()
