import json
import tempfile
import unittest
from pathlib import Path

from tools import build_app_bundle as bundle


class PackageMetadataTests(unittest.TestCase):
    def write_manifest(self, root: Path, display_name: object) -> Path:
        path = root / "app.json"
        path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "app_id": "micropixel.test",
                    "display_name": display_name,
                    "source": "unused.cpp",
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        return path

    def test_localized_display_names_serialize_as_typed_schema_v1(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(
                Path(directory),
                {
                    "default": "en",
                    "values": {"en": "Test", "zh-Hans": "测试"},
                },
            )
            app_id, names, launch = bundle.load_app_manifest(path)
            payload = json.loads(bundle.serialize_package_metadata(names))
            self.assertEqual(app_id, "micropixel.test")
            self.assertEqual(launch, "")
            self.assertEqual(payload["schema_version"], 1)
            self.assertEqual(payload["package_type"], "app")
            self.assertEqual(payload["display_name"]["default"], "en")
            self.assertEqual(payload["display_name"]["values"]["zh-Hans"], "测试")

    def test_legacy_manifest_string_is_promoted_to_one_locale(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(Path(directory), "Test")
            _, names, _ = bundle.load_app_manifest(path)
            self.assertEqual(names.default_locale, "en")
            self.assertEqual(names.values, {"en": "Test"})

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
                        "display_name": {"default": "en", "values": {"en": "Chinese Fonts"}},
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


if __name__ == "__main__":
    unittest.main()
