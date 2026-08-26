import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_font_cbin", ROOT / "tools/build_font_cbin.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class BuildFontCbinTest(unittest.TestCase):
    def test_header_records_payload_profile_and_hashes(self) -> None:
        payload = bytes(range(64))
        charset = b"U+0020..U+007E\n"
        package = MODULE.build_package(payload, "latin-fixture-v1", 18, charset)
        self.assertEqual(len(package), MODULE.HEADER_SIZE + len(payload))
        fields = MODULE.HEADER.unpack(package[: MODULE.HEADER_SIZE])
        self.assertEqual(fields[0], MODULE.MAGIC)
        self.assertEqual(fields[1:3], (MODULE.HEADER_VERSION, MODULE.HEADER_SIZE))
        self.assertEqual(fields[3:7], (len(package), MODULE.HEADER_SIZE, len(payload), MODULE.FORMAT_LVGL_CBIN_V1))
        self.assertEqual(fields[7:10], MODULE.LVGL_VERSION)
        self.assertEqual(fields[10:13], (MODULE.ENDIAN_LITTLE, MODULE.POINTER_SIZE, MODULE.GLYPH_DSC_LARGE))
        self.assertEqual(fields[14], 18)
        self.assertEqual(fields[16].rstrip(b"\0"), b"latin-fixture-v1")
        self.assertEqual(fields[17], hashlib.sha256(charset).digest())
        self.assertEqual(fields[18], hashlib.sha256(payload).digest())
        self.assertEqual(package[MODULE.HEADER_SIZE :], payload)

    def test_rejects_invalid_profile_and_empty_payload(self) -> None:
        for profile in ("", "contains space", "x" * 32, "中文"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                MODULE.build_package(b"payload", profile, 18, b"charset")
        with self.assertRaises(ValueError):
            MODULE.build_package(b"", "fixture-v1", 18, b"charset")
        with self.assertRaises(ValueError):
            MODULE.build_package(b"payload", "fixture-v1", 0, b"charset")

    def test_cli_writes_generic_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            raw = directory / "font.cbin"
            charset = directory / "covered.txt"
            output = directory / "font.mpxcbin"
            raw.write_bytes(b"raw-cbin")
            charset.write_text("U+0041\n", encoding="utf-8")
            package = MODULE.build_package(raw.read_bytes(), "fixture-v1", 18, charset.read_bytes())
            output.write_bytes(package)
            self.assertEqual(output.read_bytes()[MODULE.HEADER_SIZE :], b"raw-cbin")


if __name__ == "__main__":
    unittest.main()
