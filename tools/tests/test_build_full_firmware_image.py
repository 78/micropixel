import unittest

from tools.build_full_firmware_image import flash_size_bytes


class FlashSizeBytesTests(unittest.TestCase):
    def test_reads_board_flash_capacity(self) -> None:
        self.assertEqual(
            flash_size_bytes({"flash_settings": {"flash_size": "16MB"}}),
            16 * 1024 * 1024,
        )
        self.assertEqual(
            flash_size_bytes({"flash_settings": {"flash_size": "32MB"}}),
            32 * 1024 * 1024,
        )

    def test_rejects_missing_or_unknown_capacity(self) -> None:
        with self.assertRaisesRegex(SystemExit, "flash_settings.flash_size"):
            flash_size_bytes({})
        with self.assertRaisesRegex(SystemExit, "unsupported flash size"):
            flash_size_bytes({"flash_settings": {"flash_size": "auto"}})


if __name__ == "__main__":
    unittest.main()
