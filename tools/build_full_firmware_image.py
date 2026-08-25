#!/usr/bin/env python3
"""Create a sparse, browser-flashable image from ESP-IDF flasher_args.json."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


FLASH_SIZE_BYTES = 32 * 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_ENTRY_SIZE = 32
PARTITION_TABLE_BINARY_SIZE = 0xC00


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--app-store-image", type=Path)
    return parser.parse_args()


def find_partition(partition_table: bytes, label: str) -> tuple[int, int]:
    for cursor in range(0, PARTITION_TABLE_BINARY_SIZE, PARTITION_ENTRY_SIZE):
        entry = partition_table[cursor : cursor + PARTITION_ENTRY_SIZE]
        if entry == b"\xff" * PARTITION_ENTRY_SIZE:
            break
        if entry[:2] == b"\xeb\xeb":
            continue
        if entry[:2] != PARTITION_MAGIC:
            raise SystemExit(f"invalid partition table entry at 0x{PARTITION_TABLE_OFFSET + cursor:x}")
        partition_label = entry[12:28].split(b"\0", 1)[0].decode("ascii")
        if partition_label == label:
            return struct.unpack_from("<II", entry, 4)
    raise SystemExit(f"partition table does not contain {label}")


def main() -> None:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    configuration = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    flash_files = configuration.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise SystemExit("flasher_args.json does not contain flash_files")

    regions: list[tuple[int, int, Path, bytes]] = []
    for raw_offset, raw_file in flash_files.items():
        if not isinstance(raw_offset, str) or not isinstance(raw_file, str):
            raise SystemExit("flash_files entries must map string offsets to paths")
        offset = int(raw_offset, 0)
        source = (build_dir / raw_file).resolve()
        if build_dir not in source.parents or not source.is_file():
            raise SystemExit(f"flash input is missing or outside the build directory: {raw_file}")
        data = source.read_bytes()
        end = offset + len(data)
        if offset < 0 or not data or end > FLASH_SIZE_BYTES:
            raise SystemExit(f"invalid flash region: {raw_offset} {raw_file}")
        regions.append((offset, end, source, data))

    partition_region = next((region for region in regions if region[0] == PARTITION_TABLE_OFFSET), None)
    if partition_region is None or partition_region[3][:2] != PARTITION_MAGIC:
        raise SystemExit("flasher_args.json does not contain an ESP partition table at 0x8000")

    if args.app_store_image is not None:
        app_store_path = args.app_store_image.resolve()
        if not app_store_path.is_file():
            raise SystemExit(f"app_store image is missing: {app_store_path}")
        app_store_offset, app_store_size = find_partition(partition_region[3], "app_store")
        app_store_data = app_store_path.read_bytes()
        if not app_store_data or len(app_store_data) > app_store_size:
            raise SystemExit(
                f"app_store image does not fit partition ({len(app_store_data)} > {app_store_size})"
            )
        regions.append(
            (app_store_offset, app_store_offset + len(app_store_data), app_store_path, app_store_data)
        )

    regions.sort(key=lambda item: item[0])
    for previous, current in zip(regions, regions[1:]):
        if current[0] < previous[1]:
            raise SystemExit(f"overlapping flash regions: {previous[2].name} and {current[2].name}")

    image_end = (max(region[1] for region in regions) + 0xFFF) & ~0xFFF
    image = bytearray(b"\xff" * image_end)
    for offset, end, _source, data in regions:
        image[offset:end] = data
    if image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 2] != PARTITION_MAGIC:
        raise SystemExit("merged image does not contain an ESP partition table at 0x8000")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"Browser flash image: {args.output} ({len(image)} bytes, {len(regions)} regions)")


if __name__ == "__main__":
    main()
