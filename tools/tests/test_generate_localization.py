import json
import tempfile
import unittest
from pathlib import Path

from tools import generate_localization


class GenerateLocalizationTest(unittest.TestCase):
    def make_app(self, root: Path, catalogs: dict[str, dict[str, str]]) -> Path:
        i18n = root / "i18n"
        i18n.mkdir()
        translations = {}
        for locale, strings in catalogs.items():
            path = i18n / f"{locale}.json"
            path.write_text(json.dumps(strings, ensure_ascii=False), encoding="utf-8")
            translations[locale] = f"i18n/{locale}.json"
        manifest = root / "app.json"
        manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "app_id": "micropixel.test",
                    "display_name": "Test",
                    "localization": {"default": "en", "translations": translations},
                }
            ),
            encoding="utf-8",
        )
        return manifest

    def test_normalizes_supported_subset(self):
        self.assertEqual(generate_localization.normalize_locale_tag("EN-us"), "en-US")
        self.assertEqual(generate_localization.normalize_locale_tag("zh-hans-cn"), "zh-Hans-CN")
        self.assertEqual(generate_localization.normalize_locale_tag("es-419"), "es-419")
        for value in ("", "e", "en_US", "en--US", "en-US-extra", "abcd"):
            with self.assertRaises(ValueError):
                generate_localization.normalize_locale_tag(value)

    def test_generates_catalog_and_requested_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self.make_app(
                root,
                {
                    "en": {"game.title": "Snake", "game.start": "Start"},
                    "zh-Hans": {"game.title": "贪吃蛇", "game.start": "开始"},
                },
            )
            source = generate_localization.catalog_from_manifest(manifest)
            keys, catalogs = generate_localization.load_catalogs(source)
            report, fingerprint = generate_localization.make_report(source, keys, catalogs)
            header = generate_localization.generate_header(
                "test_strings", source, keys, catalogs, fingerprint
            )

            self.assertEqual(keys, ["game.start", "game.title"])
            self.assertIn("enum class Id", header)
            self.assertIn("kGameStart", header)
            self.assertIn('detail::Equal(tag, "zh-Hans")', header)
            self.assertIn("ForLocale(const micropixel::Locale& locale)", header)
            self.assertNotIn("<array>", header)
            self.assertNotIn("string_view", header)
            self.assertIn(ord("贪"), report["requested_codepoints"])
            self.assertEqual(report["catalogs"]["en"]["path"], "en.json")
            self.assertEqual(report["fingerprint"], fingerprint)

    def test_rejects_key_mismatch_and_noncanonical_locale(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self.make_app(
                root,
                {
                    "en": {"game.title": "Snake"},
                    "zh-Hans": {"game.start": "开始"},
                },
            )
            source = generate_localization.catalog_from_manifest(manifest)
            with self.assertRaisesRegex(ValueError, "catalog keys differ"):
                generate_localization.load_catalogs(source)

            value = json.loads(manifest.read_text(encoding="utf-8"))
            value["localization"]["translations"]["zh-hans"] = value["localization"][
                "translations"
            ].pop("zh-Hans")
            manifest.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "canonical spelling"):
                generate_localization.catalog_from_manifest(manifest)

    def test_rejects_enum_collisions_and_controls(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = self.make_app(
                root,
                {"en": {"game.title": "Snake", "game_title": "Other"}},
            )
            source = generate_localization.catalog_from_manifest(manifest)
            keys, catalogs = generate_localization.load_catalogs(source)
            report, fingerprint = generate_localization.make_report(source, keys, catalogs)
            self.assertTrue(report["fingerprint"])
            with self.assertRaisesRegex(ValueError, "collide"):
                generate_localization.generate_header(
                    "test_strings", source, keys, catalogs, fingerprint
                )

            (root / "i18n/en.json").write_text(
                json.dumps({"game.title": "bad\ntext"}), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "control"):
                generate_localization.load_catalogs(source)


if __name__ == "__main__":
    unittest.main()
