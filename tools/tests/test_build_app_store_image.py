#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


WORKSPACE_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = WORKSPACE_ROOT / "tools" / "build_app_store_image.py"
SPEC = importlib.util.spec_from_file_location("build_app_store_image", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
STORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STORE)


def make_bundle(
    path: Path,
    app_id: str,
    payload: bytes = b"AOT!",
    display_name: str = "Test App",
    include_metadata: bool = True,
) -> Path:
    bundle_size = STORE.EXTENT_ALIGNMENT
    metadata_offset = 256
    section_offset = 320
    app_id_bytes = app_id.encode("ascii")
    app_id_field = app_id_bytes + bytes(64 - len(app_id_bytes))
    sections = [STORE.SECTION.pack(
        STORE.KIND_AOT,
        0,
        section_offset,
        len(payload),
        STORE.fnv1a32(payload),
        STORE.FORMAT_AOT_RELOCATABLE,
        0,
        0,
        0,
        0,
        0,
        0,
    )]
    display_name_bytes = display_name.encode("utf-8")
    if include_metadata:
        sections.append(STORE.SECTION.pack(
            STORE.KIND_APP_METADATA,
            0,
            metadata_offset,
            len(display_name_bytes),
            STORE.fnv1a32(display_name_bytes),
            STORE.FORMAT_UTF8,
            0,
            0,
            0,
            0,
            0,
            0,
        ))
    header_without_hash = STORE.HEADER.pack(
        STORE.MAGIC,
        STORE.VERSION,
        STORE.HEADER_SIZE,
        bundle_size,
        STORE.HEADER_SIZE,
        app_id_field,
        len(app_id_bytes),
        len(sections),
        STORE.FRAMEWORK_ABI_VERSION,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    header = STORE.HEADER.pack(
        STORE.MAGIC,
        STORE.VERSION,
        STORE.HEADER_SIZE,
        bundle_size,
        STORE.HEADER_SIZE,
        app_id_field,
        len(app_id_bytes),
        len(sections),
        STORE.FRAMEWORK_ABI_VERSION,
        0,
        STORE.fnv1a32(header_without_hash),
        0,
        0,
        0,
        0,
        0,
    )
    image = bytearray(bundle_size)
    image[: STORE.HEADER_SIZE] = header
    for index, section in enumerate(sections):
        begin = STORE.HEADER_SIZE + index * STORE.SECTION.size
        image[begin : begin + STORE.SECTION.size] = section
    if include_metadata:
        image[metadata_offset : metadata_offset + len(display_name_bytes)] = display_name_bytes
    image[section_offset : section_offset + len(payload)] = payload
    path.write_bytes(image)
    return path


class AppStoreImageTest(unittest.TestCase):
    def test_three_bundles_are_sequential_and_terminated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = make_bundle(root / "blocks.bin", "micropixel.blocks")
            second = make_bundle(root / "snake.bin", "micropixel.snake")
            third = make_bundle(root / "demo.bin", "micropixel.demo")
            image = STORE.build_app_store([first, second, third])
        self.assertEqual(len(image), 3 * STORE.EXTENT_ALIGNMENT + STORE.TERMINATOR_SIZE)
        self.assertEqual(image[:8], STORE.MAGIC)
        self.assertEqual(image[STORE.EXTENT_ALIGNMENT : STORE.EXTENT_ALIGNMENT + 8], STORE.MAGIC)
        self.assertEqual(image[2 * STORE.EXTENT_ALIGNMENT : 2 * STORE.EXTENT_ALIGNMENT + 8], STORE.MAGIC)
        self.assertEqual(image[-STORE.TERMINATOR_SIZE :], bytes([0xFF]) * STORE.TERMINATOR_SIZE)

    def test_duplicate_app_id_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = make_bundle(root / "first.bin", "micropixel.same")
            second = make_bundle(root / "second.bin", "micropixel.same")
            with self.assertRaisesRegex(ValueError, "Duplicate AppId"):
                STORE.build_app_store([first, second])

    def test_display_name_is_read_from_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = make_bundle(Path(directory) / "named.bin", "micropixel.named", display_name="Named App")
            app_id, display_name, bundle_size = STORE.bundle_identity(bundle, bundle.read_bytes())
        self.assertEqual(app_id, "micropixel.named")
        self.assertEqual(display_name, "Named App")
        self.assertEqual(bundle_size, STORE.EXTENT_ALIGNMENT)

    def test_missing_display_name_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = make_bundle(Path(directory) / "unnamed.bin", "micropixel.unnamed", include_metadata=False)
            with self.assertRaisesRegex(ValueError, "missing.*App metadata"):
                STORE.build_app_store([bundle])

    def test_more_than_hall_capacity_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundles = [make_bundle(root / f"app-{index}.bin", f"micropixel.app{index}") for index in range(4)]
            with self.assertRaisesRegex(ValueError, "between 1 and 3"):
                STORE.build_app_store(bundles)

    def test_invalid_app_id_is_rejected_before_flash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = make_bundle(Path(directory) / "bad.bin", "micropixel.bad!")
            with self.assertRaisesRegex(ValueError, "Invalid AppId characters"):
                STORE.build_app_store([bundle])

    def test_corrupt_section_is_rejected_before_flash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = make_bundle(Path(directory) / "corrupt.bin", "micropixel.corrupt")
            data = bytearray(bundle.read_bytes())
            data[320] ^= 0xFF
            bundle.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "section 0 hash mismatch"):
                STORE.build_app_store([bundle])


if __name__ == "__main__":
    unittest.main()
