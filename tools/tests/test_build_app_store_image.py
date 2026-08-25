from __future__ import annotations

import importlib.util
import hashlib
import struct
import tempfile
import unittest
import zlib
from pathlib import Path


WORKSPACE_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = WORKSPACE_ROOT / "tools" / "build_app_store_image.py"
SPEC = importlib.util.spec_from_file_location("build_app_store_image", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
STORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STORE)


class BundleFsImageTest(unittest.TestCase):
    def write_bundle(self, path: Path, app_id: str) -> bytes:
        data = bytearray(STORE.DATA_BLOCK_SIZE)
        encoded_id = app_id.encode("ascii")
        STORE.BUNDLE_HEADER.pack_into(
            data,
            0,
            STORE.BUNDLE_MAGIC,
            STORE.BUNDLE_VERSION,
            STORE.BUNDLE_HEADER_SIZE,
            len(data),
            STORE.BUNDLE_HEADER_SIZE,
            encoded_id + bytes(64 - len(encoded_id)),
            len(encoded_id),
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
        )
        path.write_bytes(data)
        return bytes(data)

    def test_empty_image_contains_one_committed_bank(self) -> None:
        image = STORE.build_empty_bundlefs()

        self.assertEqual(len(image), STORE.METADATA_SIZE)
        fields = STORE.CATALOG_HEADER.unpack_from(image)
        self.assertEqual(fields[0], STORE.CATALOG_MAGIC)
        self.assertEqual(fields[1], STORE.FORMAT_VERSION)
        self.assertEqual(fields[2], STORE.HEADER_SIZE)
        self.assertEqual(fields[3], STORE.BANK_SIZE)
        self.assertEqual(fields[4], 1)
        self.assertEqual(fields[5], 0)
        self.assertEqual(fields[6], STORE.BANK_COUNT)
        self.assertEqual(fields[7], STORE.BANK_SIZE)
        self.assertEqual(fields[8], STORE.METADATA_SIZE)
        self.assertEqual(fields[9], STORE.DATA_OFFSET)
        self.assertEqual(fields[10], STORE.DATA_BLOCK_SIZE)
        self.assertEqual(fields[11], STORE.APP_STORE_SIZE)
        self.assertEqual(fields[12], 383)
        self.assertEqual(struct.unpack_from("<I", image, STORE.COMMIT_OFFSET)[0], STORE.COMMIT_MARKER)
        self.assertEqual(image[STORE.BANK_SIZE :], b"\xff" * (STORE.METADATA_SIZE - STORE.BANK_SIZE))

    def test_bank_checksum_covers_erased_commit_marker(self) -> None:
        image = bytearray(STORE.build_empty_bundlefs())
        expected = struct.unpack_from("<I", image, STORE.CHECKSUM_OFFSET)[0]
        struct.pack_into("<I", image, STORE.CHECKSUM_OFFSET, 0)
        struct.pack_into("<I", image, STORE.COMMIT_OFFSET, 0xFFFFFFFF)
        self.assertEqual(zlib.crc32(image[: STORE.BANK_SIZE]) & 0xFFFFFFFF, expected)

    def test_geometry_uses_four_banks_without_slots(self) -> None:
        self.assertEqual(STORE.BANK_COUNT, 4)
        self.assertEqual(STORE.BANK_SIZE, 16384)
        self.assertEqual(STORE.MAX_FILES, 50)
        self.assertEqual(STORE.DATA_OFFSET, 65536)
        self.assertEqual(STORE.DATA_BLOCK_COUNT, 383)
        self.assertFalse(hasattr(STORE, "SLOT_SIZE"))

    def test_seeded_image_contains_bundle_catalog_and_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "demo.bundle.bin"
            data = bytearray(STORE.DATA_BLOCK_SIZE)
            STORE.BUNDLE_HEADER.pack_into(
                data,
                0,
                STORE.BUNDLE_MAGIC,
                STORE.BUNDLE_VERSION,
                STORE.BUNDLE_HEADER_SIZE,
                len(data),
                STORE.BUNDLE_HEADER_SIZE,
                b"demo" + bytes(60),
                4,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            )
            path.write_bytes(data)

            image = STORE.build_bundlefs([path])
            fields = STORE.CATALOG_HEADER.unpack_from(image)
            self.assertEqual(fields[13:16], (1, 1, 1))
            entry = image[STORE.CATALOG_ENTRIES_OFFSET : STORE.CATALOG_ENTRIES_OFFSET + STORE.CATALOG_ENTRY_SIZE]
            self.assertEqual(entry[:5], b"demo\0")
            self.assertEqual(struct.unpack_from("<IIHH", entry, 68), (len(data),
                             int.from_bytes(hashlib.sha256(data).digest()[:4], "big"), 0, 1))
            self.assertEqual(entry[80:112], hashlib.sha256(data).digest())
            self.assertEqual(image[STORE.DATA_OFFSET:], data)

    def test_seeded_image_rejects_duplicate_app_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / "first.bundle.bin", Path(directory) / "second.bundle.bin"]
            for path in paths:
                data = bytearray(STORE.DATA_BLOCK_SIZE)
                STORE.BUNDLE_HEADER.pack_into(
                    data, 0, STORE.BUNDLE_MAGIC, STORE.BUNDLE_VERSION, STORE.BUNDLE_HEADER_SIZE, len(data),
                    STORE.BUNDLE_HEADER_SIZE, b"same" + bytes(60), 4, 1, 1, 0, 0, 0, 0, 0, 0, 0,
                )
                path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Duplicate AppId"):
                STORE.build_bundlefs(paths)

    def test_seeded_image_preserves_explicit_hall_order(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            app_ids = ["orbit", "sketch", "colors", "counter", "blocks", "snake", "demo"]
            paths = []
            for app_id in app_ids:
                path = Path(directory) / f"{app_id}.bundle.bin"
                self.write_bundle(path, app_id)
                paths.append(path)

            image = STORE.build_bundlefs(paths)
            fields = STORE.CATALOG_HEADER.unpack_from(image)
            self.assertEqual(fields[14], len(app_ids))
            actual_ids = []
            for index in range(len(app_ids)):
                offset = STORE.CATALOG_ENTRIES_OFFSET + index * STORE.CATALOG_ENTRY_SIZE
                entry = image[offset : offset + STORE.CATALOG_ENTRY_SIZE]
                actual_ids.append(entry[: STORE.CATALOG_ENTRY_SIZE].split(b"\0", 1)[0].decode("ascii"))
            self.assertEqual(actual_ids, app_ids)


if __name__ == "__main__":
    unittest.main()
