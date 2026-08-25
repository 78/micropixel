#!/usr/bin/env python3
"""Build a MicroPixel BundleFS image, optionally seeded with App Bundles."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import zlib
from pathlib import Path


CATALOG_MAGIC = b"MPBUNDLE"
FORMAT_VERSION = 2
BANK_SIZE = 16 * 1024
BANK_COUNT = 4
METADATA_SIZE = 64 * 1024
DATA_OFFSET = METADATA_SIZE
DATA_BLOCK_SIZE = 64 * 1024
APP_STORE_SIZE = 24 * 1024 * 1024
DATA_BLOCK_COUNT = (APP_STORE_SIZE - DATA_OFFSET) // DATA_BLOCK_SIZE
MAX_FILES = 50
CATALOG_ENTRY_SIZE = 112
BLOCK_NUMBER_SIZE = 2
HEADER_SIZE = 64
CHECKSUM_OFFSET = BANK_SIZE - 8
COMMIT_OFFSET = BANK_SIZE - 4
COMMIT_MARKER = 0x434F4D54
CATALOG_HEADER = struct.Struct("<8sHHIQHHIIIIIHHHHII")
BUNDLE_HEADER = struct.Struct("<8sIIII64sIIIIIIIIII")
BUNDLE_MAGIC = b"MPXBNDL\0"
BUNDLE_VERSION = 1
BUNDLE_HEADER_SIZE = 128
BUNDLE_APP_ID_LENGTH_OFFSET = 88
CATALOG_ENTRIES_OFFSET = HEADER_SIZE
CATALOG_BLOCK_MAP_OFFSET = CATALOG_ENTRIES_OFFSET + MAX_FILES * CATALOG_ENTRY_SIZE


def bundle_identity(path: Path, data: bytes) -> str:
    if len(data) < BUNDLE_HEADER_SIZE:
        raise ValueError(f"Bundle is shorter than its header: {path}")
    fields = BUNDLE_HEADER.unpack_from(data)
    magic, version, header_size, bundle_size = fields[:4]
    app_id_bytes = fields[5]
    app_id_length = fields[6]
    if magic != BUNDLE_MAGIC or version != BUNDLE_VERSION or header_size != BUNDLE_HEADER_SIZE:
        raise ValueError(f"Unsupported MicroPixel Bundle: {path}")
    if bundle_size != len(data) or bundle_size == 0 or bundle_size % DATA_BLOCK_SIZE != 0:
        raise ValueError(f"Bundle size is not exactly 64 KiB aligned: {path}")
    if app_id_length <= 0 or app_id_length > len(app_id_bytes) or any(app_id_bytes[app_id_length:]):
        raise ValueError(f"Invalid AppId length in Bundle: {path}")
    try:
        app_id = app_id_bytes[:app_id_length].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError(f"AppId is not ASCII in Bundle: {path}") from error
    if re.fullmatch(r"[A-Za-z0-9_.-]+", app_id) is None:
        raise ValueError(f"Invalid AppId characters in Bundle: {path}")
    return app_id


def build_empty_bundlefs() -> bytes:
    """Return the 64 KiB metadata region for an empty BundleFS v2."""
    if DATA_BLOCK_COUNT != 383 or CATALOG_HEADER.size != HEADER_SIZE:
        raise AssertionError("BundleFS v2 geometry changed without updating the image builder")

    bank = bytearray(BANK_SIZE)
    payload_size = MAX_FILES * CATALOG_ENTRY_SIZE + DATA_BLOCK_COUNT * BLOCK_NUMBER_SIZE
    CATALOG_HEADER.pack_into(
        bank,
        0,
        CATALOG_MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        BANK_SIZE,
        1,
        0,
        BANK_COUNT,
        BANK_SIZE,
        METADATA_SIZE,
        DATA_OFFSET,
        DATA_BLOCK_SIZE,
        APP_STORE_SIZE,
        DATA_BLOCK_COUNT,
        0,
        0,
        0,
        0,
        payload_size,
    )
    struct.pack_into("<I", bank, CHECKSUM_OFFSET, 0)
    struct.pack_into("<I", bank, COMMIT_OFFSET, 0xFFFFFFFF)
    checksum = zlib.crc32(bank) & 0xFFFFFFFF
    struct.pack_into("<I", bank, CHECKSUM_OFFSET, checksum)
    struct.pack_into("<I", bank, COMMIT_OFFSET, COMMIT_MARKER)

    image = bytearray(b"\xff" * METADATA_SIZE)
    image[:BANK_SIZE] = bank
    return bytes(image)


def build_bundlefs(bundle_paths: list[Path]) -> bytes:
    """Return a fresh BundleFS image containing the requested Bundles."""
    if not bundle_paths:
        return build_empty_bundlefs()
    if len(bundle_paths) > MAX_FILES:
        raise ValueError(f"BundleFS supports at most {MAX_FILES} Apps")

    bank = bytearray(BANK_SIZE)
    entries = bytearray(MAX_FILES * CATALOG_ENTRY_SIZE)
    block_map = bytearray(DATA_BLOCK_COUNT * BLOCK_NUMBER_SIZE)
    data_region = bytearray()
    app_ids: set[str] = set()
    next_block = 0

    for index, bundle_path in enumerate(bundle_paths):
        data = bundle_path.read_bytes()
        app_id = bundle_identity(bundle_path, data)
        if app_id in app_ids:
            raise ValueError(f"Duplicate AppId in BundleFS image: {app_id}")
        app_ids.add(app_id)
        block_count = len(data) // DATA_BLOCK_SIZE
        if next_block + block_count > DATA_BLOCK_COUNT:
            raise ValueError("Bundles do not fit in the app_store partition")

        digest = hashlib.sha256(data).digest()
        entry_offset = index * CATALOG_ENTRY_SIZE
        encoded_name = app_id.encode("ascii")
        entries[entry_offset : entry_offset + len(encoded_name)] = encoded_name
        struct.pack_into("<IIHH", entries, entry_offset + 68, len(data), int.from_bytes(digest[:4], "big"),
                         next_block, block_count)
        entries[entry_offset + 80 : entry_offset + 112] = digest
        for logical_block in range(block_count):
            struct.pack_into("<H", block_map, (next_block + logical_block) * BLOCK_NUMBER_SIZE,
                             next_block + logical_block)
        data_region.extend(data)
        print(
            f"app={app_id} blocks={next_block}..{next_block + block_count - 1} "
            f"size={len(data)} sha256={digest.hex()}"
        )
        next_block += block_count

    payload_size = MAX_FILES * CATALOG_ENTRY_SIZE + DATA_BLOCK_COUNT * BLOCK_NUMBER_SIZE
    CATALOG_HEADER.pack_into(
        bank,
        0,
        CATALOG_MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        BANK_SIZE,
        1,
        0,
        BANK_COUNT,
        BANK_SIZE,
        METADATA_SIZE,
        DATA_OFFSET,
        DATA_BLOCK_SIZE,
        APP_STORE_SIZE,
        DATA_BLOCK_COUNT,
        next_block % DATA_BLOCK_COUNT,
        len(bundle_paths),
        next_block,
        0,
        payload_size,
    )
    bank[CATALOG_ENTRIES_OFFSET : CATALOG_BLOCK_MAP_OFFSET] = entries
    bank[CATALOG_BLOCK_MAP_OFFSET : CATALOG_BLOCK_MAP_OFFSET + len(block_map)] = block_map
    struct.pack_into("<I", bank, CHECKSUM_OFFSET, 0)
    struct.pack_into("<I", bank, COMMIT_OFFSET, 0xFFFFFFFF)
    checksum = zlib.crc32(bank) & 0xFFFFFFFF
    struct.pack_into("<I", bank, CHECKSUM_OFFSET, checksum)
    struct.pack_into("<I", bank, COMMIT_OFFSET, COMMIT_MARKER)

    image = bytearray(b"\xff" * METADATA_SIZE)
    image[:BANK_SIZE] = bank
    image.extend(data_region)
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("bundles", nargs="*", type=Path)
    args = parser.parse_args()

    image = build_bundlefs(args.bundles)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    contents = "empty" if not args.bundles else f"{len(args.bundles)} Apps"
    print(f"BundleFS image: {args.output} ({len(image)} bytes, {contents})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
