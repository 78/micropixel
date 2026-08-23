#!/usr/bin/env python3
"""Build a sequential MicroPixel App Store image from aligned Bundle extents."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


MAGIC = b"MPXBNDL\0"
VERSION = 1
FRAMEWORK_ABI_VERSION = 1
EXTENT_ALIGNMENT = 64 * 1024
HEADER_SIZE = 128
TERMINATOR_SIZE = 4096
SECTION_ALIGNMENT = 64
MAX_APPS = 3
MAX_SECTIONS = 128
DISPLAY_NAME_MAX_LENGTH = 64
KIND_AOT = 1
KIND_ASSET = 2
KIND_APP_METADATA = 3
FORMAT_AOT_RELOCATABLE = 1
FORMAT_RAW_RGB888 = 2
FORMAT_PNG = 4
FORMAT_RAW_ARGB8888 = 5
FORMAT_UTF8 = 6
HEADER = struct.Struct("<8sIIII64sIIIIIIIIII")
SECTION = struct.Struct("<IIIIIIIIIIII")


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def valid_display_name(data: bytes, path: Path) -> str:
    if not data or len(data) > DISPLAY_NAME_MAX_LENGTH:
        raise ValueError(f"Invalid display name length in Bundle: {path}")
    try:
        display_name = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"Display name is not UTF-8 in Bundle: {path}") from error
    if (
        display_name.startswith(" ")
        or display_name.endswith(" ")
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in display_name)
    ):
        raise ValueError(f"Invalid display name characters in Bundle: {path}")
    return display_name


def bundle_identity(path: Path, data: bytes) -> tuple[str, str, int]:
    if len(data) < HEADER_SIZE:
        raise ValueError(f"Bundle is shorter than its header: {path}")
    fields = HEADER.unpack_from(data)
    magic, version, header_size, bundle_size, toc_offset = fields[:5]
    app_id_bytes = fields[5]
    app_id_length = fields[6]
    section_count = fields[7]
    framework_abi_version = fields[8]
    launch_asset_id = fields[9]
    expected_header_hash = fields[10]
    reserved = fields[11:]
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise ValueError(f"Unsupported MicroPixel Bundle: {path}")
    if bundle_size != len(data) or bundle_size % EXTENT_ALIGNMENT != 0:
        raise ValueError(f"Bundle extent is not exactly 64 KiB aligned: {path}")
    if (
        app_id_length <= 0
        or app_id_length > len(app_id_bytes)
        or any(app_id_bytes[app_id_length:])
    ):
        raise ValueError(f"Invalid AppId length in Bundle: {path}")
    try:
        app_id = app_id_bytes[:app_id_length].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError(f"AppId is not ASCII in Bundle: {path}") from error
    if re.fullmatch(r"[A-Za-z0-9_.-]+", app_id) is None:
        raise ValueError(f"Invalid AppId characters in Bundle: {path}")
    if (
        framework_abi_version != FRAMEWORK_ABI_VERSION
        or toc_offset != HEADER_SIZE
        or section_count <= 0
        or section_count > MAX_SECTIONS
        or toc_offset + section_count * SECTION.size > bundle_size
        or any(reserved)
    ):
        raise ValueError(f"Invalid Bundle header fields: {path}")
    header_without_hash = bytearray(data[:HEADER_SIZE])
    struct.pack_into("<I", header_without_hash, 104, 0)
    if fnv1a32(header_without_hash) != expected_header_hash:
        raise ValueError(f"Bundle header hash mismatch: {path}")

    sections: list[tuple[int, ...]] = []
    aot_found = False
    metadata_found = False
    display_name = app_id
    launch_found = launch_asset_id == 0
    asset_ids: set[int] = set()
    toc_end = toc_offset + section_count * SECTION.size
    for index in range(section_count):
        section = SECTION.unpack_from(data, toc_offset + index * SECTION.size)
        kind, section_id, offset, size, content_hash, fmt, width, height, stride, flags, reserved0, reserved1 = section
        if (
            size <= 0
            or offset < toc_end
            or offset % SECTION_ALIGNMENT != 0
            or offset > bundle_size
            or size > bundle_size - offset
            or flags != 0
            or reserved0 != 0
            or reserved1 != 0
        ):
            raise ValueError(f"Invalid Bundle section {index}: {path}")
        end = offset + size
        if any(offset < other[3] and other[2] < end for other in sections):
            raise ValueError(f"Overlapping Bundle section {index}: {path}")
        if fnv1a32(data[offset:end]) != content_hash:
            raise ValueError(f"Bundle section {index} hash mismatch: {path}")

        if kind == KIND_AOT:
            if (
                aot_found
                or section_id != 0
                or fmt != FORMAT_AOT_RELOCATABLE
                or width != 0
                or height != 0
                or stride != 0
            ):
                raise ValueError(f"Invalid or duplicate AOT section: {path}")
            aot_found = True
        elif kind == KIND_ASSET:
            if section_id <= 0 or section_id in asset_ids or not FORMAT_RAW_RGB888 <= fmt <= FORMAT_RAW_ARGB8888:
                raise ValueError(f"Invalid or duplicate asset section {index}: {path}")
            if width <= 0 or height <= 0:
                raise ValueError(f"Invalid asset dimensions in section {index}: {path}")
            if fmt == FORMAT_RAW_RGB888 and (stride != width * 3 or stride * height != size):
                raise ValueError(f"Invalid RGB888 asset section {index}: {path}")
            if fmt == FORMAT_RAW_ARGB8888 and (stride != width * 4 or stride * height != size):
                raise ValueError(f"Invalid ARGB8888 asset section {index}: {path}")
            asset_ids.add(section_id)
            if section_id == launch_asset_id:
                if fmt not in (FORMAT_RAW_RGB888, FORMAT_PNG):
                    raise ValueError(f"Launch asset must be raw RGB888 or PNG: {path}")
                launch_found = True
        elif kind == KIND_APP_METADATA:
            if (
                metadata_found
                or section_id != 0
                or fmt != FORMAT_UTF8
                or width != 0
                or height != 0
                or stride != 0
            ):
                raise ValueError(f"Invalid or duplicate App metadata section: {path}")
            display_name = valid_display_name(data[offset:end], path)
            metadata_found = True
        else:
            raise ValueError(f"Unknown Bundle section kind {kind}: {path}")
        sections.append((kind, section_id, offset, end))

    if not aot_found or not launch_found or not metadata_found:
        raise ValueError(f"Bundle is missing its AOT, launch or App metadata section: {path}")
    return app_id, display_name, bundle_size


def build_app_store(bundle_paths: list[Path]) -> bytes:
    if not bundle_paths or len(bundle_paths) > MAX_APPS:
        raise ValueError(f"App Store requires between 1 and {MAX_APPS} Bundles")

    image = bytearray()
    app_ids: set[str] = set()
    for bundle_path in bundle_paths:
        data = bundle_path.read_bytes()
        app_id, display_name, bundle_size = bundle_identity(bundle_path, data)
        if app_id in app_ids:
            raise ValueError(f"Duplicate AppId in App Store image: {app_id}")
        app_ids.add(app_id)
        print(
            f"slot={len(app_ids) - 1} offset=0x{len(image):x} size=0x{bundle_size:x} "
            f"app={app_id} title={display_name!r}"
        )
        image.extend(data)

    # Force the header immediately after the final Bundle back to erased Flash
    # so a previously longer catalog cannot leak into the new scan.
    image.extend(b"\xff" * TERMINATOR_SIZE)
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("bundles", nargs="+", type=Path)
    args = parser.parse_args()

    image = build_app_store(args.bundles)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"App Store image: {args.output} ({len(image)} bytes, {len(args.bundles)} apps)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
