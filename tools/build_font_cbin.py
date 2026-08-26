#!/usr/bin/env python3
"""Wrap a pinned lv_font_conv cbin payload in MicroPixel's validated header."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


MAGIC = b"MPXFCBN\0"
HEADER_VERSION = 1
HEADER_SIZE = 160
FORMAT_LVGL_CBIN_V1 = 1
ENDIAN_LITTLE = 1
POINTER_SIZE = 4
GLYPH_DSC_LARGE = 1
LVGL_VERSION = (9, 5, 0)
CONVERTER_COMMIT = "c420999fe79adb0bc2a480c4a64fd33fc6e34519"
MAX_PACKAGE_SIZE = 16 * 1024 * 1024
HEADER = struct.Struct("<8sHHIIIIHHHBBBBH20s32s32s32s4s")


def checked_profile(value: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("font profile must contain printable ASCII only") from error
    if not encoded or len(encoded) >= 32 or any(byte < 0x21 or byte > 0x7E for byte in encoded):
        raise ValueError("font profile must be 1..31 printable ASCII bytes without spaces")
    return encoded + bytes(32 - len(encoded))


def build_package(payload: bytes, profile: str, size: int, charset_source: bytes) -> bytes:
    total_size = HEADER_SIZE + len(payload)
    if not payload:
        raise ValueError("raw cbin payload must not be empty")
    if total_size > MAX_PACKAGE_SIZE:
        raise ValueError(f"font cbin package exceeds {MAX_PACKAGE_SIZE} bytes")
    if size <= 0 or size > 4096:
        raise ValueError("font size must be in range 1..4096 pixels")
    header = HEADER.pack(
        MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        total_size,
        HEADER_SIZE,
        len(payload),
        FORMAT_LVGL_CBIN_V1,
        *LVGL_VERSION,
        ENDIAN_LITTLE,
        POINTER_SIZE,
        GLYPH_DSC_LARGE,
        0,
        size,
        bytes.fromhex(CONVERTER_COMMIT),
        checked_profile(profile),
        hashlib.sha256(charset_source).digest(),
        hashlib.sha256(payload).digest(),
        bytes(4),
    )
    if len(header) != HEADER_SIZE:
        raise AssertionError("MicroPixel font cbin header size drifted")
    return header + payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-cbin", type=Path, required=True, help="raw cbin from the pinned 78/lv_font_conv")
    parser.add_argument("--profile", required=True, help="stable profile/fingerprint, maximum 31 printable ASCII bytes")
    parser.add_argument("--size", type=int, required=True, help="nominal font size in pixels")
    parser.add_argument("--charset-source", type=Path, required=True, help="canonical charset/report hashed into the header")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not args.raw_cbin.is_file() or not args.charset_source.is_file():
        parser.error("--raw-cbin and --charset-source must name regular files")
    try:
        package = build_package(args.raw_cbin.read_bytes(), args.profile, args.size, args.charset_source.read_bytes())
    except ValueError as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(package)
    print(
        f"Font cbin v1: profile={args.profile} payload={len(package) - HEADER_SIZE} "
        f"total={len(package)} converter={CONVERTER_COMMIT} -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
