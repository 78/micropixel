import json
import struct
import tempfile
import unittest
from pathlib import Path

from tools import build_app_bundle as bundle


class PackageMetadataTests(unittest.TestCase):
    @staticmethod
    def ogg_page(
        serial: int,
        sequence: int,
        header_type: int,
        granule: int,
        packets: list[bytes],
    ) -> bytes:
        laces = bytearray()
        body = bytearray()
        for packet in packets:
            remaining = len(packet)
            cursor = 0
            while remaining >= 255:
                laces.append(255)
                body.extend(packet[cursor : cursor + 255])
                cursor += 255
                remaining -= 255
            laces.append(remaining)
            body.extend(packet[cursor:])
        header = bytearray(
            b"OggS"
            + bytes((0, header_type))
            + struct.pack("<QII", granule, serial, sequence)
            + bytes(4)
            + bytes((len(laces),))
            + laces
        )
        page = header + body
        struct.pack_into("<I", page, 22, bundle.ogg_crc32(page))
        return bytes(page)

    @classmethod
    def minimal_ogg_opus(cls, channels: int = 1) -> bytes:
        serial = 0x10203040
        head = b"OpusHead" + bytes((1, channels)) + struct.pack("<HIhB", 312, 48000, 0, 0)
        tags = b"OpusTags" + struct.pack("<I", 0) + struct.pack("<I", 0)
        audio = b"\xf8\xff\xfe"
        return b"".join(
            (
                cls.ogg_page(serial, 0, 0x02, 0, [head]),
                cls.ogg_page(serial, 1, 0, 0, [tags]),
                cls.ogg_page(serial, 2, 0x04, 1272, [audio]),
            )
        )

    def write_manifest(self, root: Path, title: object) -> Path:
        path = root / "app.json"
        path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "app_id": "micropixel.test",
                    "title": title,
                    "sources": ["unused.cpp"],
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        return path

    def test_localized_titles_serialize_to_bundle_metadata_schema_v1(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(
                Path(directory),
                {
                    "default": "en",
                    "values": {"en": "Test", "zh-Hans": "测试"},
                },
            )
            manifest = bundle.load_package_manifest(path)
            app_id, titles, launch = bundle.load_app_manifest(path)
            payload = json.loads(bundle.serialize_package_metadata(manifest))
            self.assertEqual(app_id, "micropixel.test")
            self.assertEqual(launch, "")
            self.assertEqual(payload["schema_version"], 1)
            self.assertEqual(payload["package_type"], "app")
            self.assertNotIn("display", payload)
            self.assertEqual(payload["display_name"]["default"], "en")
            self.assertEqual(payload["display_name"]["values"]["zh-Hans"], "测试")
            self.assertEqual(titles.values["zh-Hans"], "测试")

    def test_title_string_is_promoted_to_one_locale(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(Path(directory), "Test")
            _, titles, _ = bundle.load_app_manifest(path)
            self.assertEqual(titles.default_locale, "en")
            self.assertEqual(titles.values, {"en": "Test"})

    def test_default_locale_requires_a_value(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(
                Path(directory),
                {"default": "en", "values": {"zh-Hans": "测试"}},
            )
            with self.assertRaisesRegex(ValueError, "has no value"):
                bundle.load_app_manifest(path)

    def test_locale_keys_must_be_canonical(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(
                Path(directory),
                {"default": "zh-Hans", "values": {"zh-hans": "测试"}},
            )
            with self.assertRaisesRegex(ValueError, "canonical spelling"):
                bundle.load_app_manifest(path)

    def test_font_component_uses_first_public_schema(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = root / "component.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "package_type": "component",
                        "component_type": "font",
                        "id": "fonts.zh-hans",
                        "title": {"default": "en", "values": {"en": "Chinese Fonts"}},
                        "version": "1.0.0",
                        "languages": ["zh-CN", "zh-Hans"],
                        "font_bundle": "noto-zh-hans-v1",
                        "charset": "system-common-v1",
                        "fonts": {
                            role: {"asset": f"font.{role}", "style": "regular", "size": size, "bpp": 4}
                            for role, size in (("small", 14), ("medium", 18), ("large", 24), ("title", 32))
                        },
                    }
                ),
                encoding="utf-8",
            )
            manifest = bundle.load_package_manifest(manifest_path)
            payload = json.loads(bundle.serialize_component_metadata(manifest))
            self.assertEqual(payload["schema_version"], 1)
            self.assertEqual(payload["package_type"], "component")
            self.assertNotIn("metadata_version", payload)

    def test_raw_pixel_assets_are_supported_but_rejected_as_launch_cover(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rgb = root / "rgb.bin"
            argb = root / "argb.bin"
            jpeg = root / "cover.jpg"
            rgb.write_bytes(bytes(12))
            argb.write_bytes(bytes(16))
            jpeg.write_bytes(
                b"\xff\xd8\xff\xc0\x00\x11\x08\x00\x02\x00\x02\x03" + bytes(9)
            )

            rgb_section = bundle.parse_asset(f"1:raw_rgb888:2:2:{rgb}")
            argb_section = bundle.parse_asset(f"2:raw_argb8888:2:2:{argb}")
            self.assertEqual((rgb_section.format, rgb_section.stride), (bundle.FORMATS["raw_rgb888"], 6))
            self.assertEqual((argb_section.format, argb_section.stride), (bundle.FORMATS["raw_argb8888"], 8))

            launch = bundle.InputSection(
                rgb_section.kind,
                rgb_section.section_id,
                rgb_section.format,
                rgb_section.width,
                rgb_section.height,
                rgb_section.stride,
                rgb_section.data,
                "launch",
            )
            with self.assertRaisesRegex(ValueError, "launch_asset must be JPEG or PNG"):
                bundle.build_resource_pack([launch], "launch")

            jpeg_section = bundle.parse_asset(f"3:jpeg:0:0:{jpeg}")
            jpeg_launch = bundle.InputSection(
                jpeg_section.kind,
                jpeg_section.section_id,
                jpeg_section.format,
                jpeg_section.width,
                jpeg_section.height,
                jpeg_section.stride,
                jpeg_section.data,
                "launch",
            )
            self.assertEqual(bundle.build_resource_pack([jpeg_launch], "launch").launch_asset_id, 3)

    def test_ogg_opus_asset_is_validated_and_keeps_zero_dimensions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "music.opus"
            path.write_bytes(self.minimal_ogg_opus(channels=2))

            section = bundle.parse_asset(f"9:ogg_opus:0:0:{path}")

            self.assertEqual(section.format, bundle.FORMATS["ogg_opus"])
            self.assertEqual((section.width, section.height, section.stride), (0, 0, 0))
            self.assertEqual(bundle.validate_ogg_opus(section.data), (2, 960))

    def test_ogg_opus_rejects_bad_crc_and_vorbis_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            corrupt = bytearray(self.minimal_ogg_opus())
            corrupt[-1] ^= 0x01
            corrupt_path = root / "corrupt.ogg"
            corrupt_path.write_bytes(corrupt)
            with self.assertRaisesRegex(ValueError, "CRC"):
                bundle.parse_asset(f"10:ogg_opus:0:0:{corrupt_path}")

            vorbis_path = root / "vorbis.ogg"
            vorbis_path.write_bytes(
                self.ogg_page(1, 0, 0x06, 0, [b"\x01vorbis" + bytes(20)])
            )
            with self.assertRaisesRegex(ValueError, "OpusHead"):
                bundle.parse_asset(f"11:ogg_opus:0:0:{vorbis_path}")


if __name__ == "__main__":
    unittest.main()
