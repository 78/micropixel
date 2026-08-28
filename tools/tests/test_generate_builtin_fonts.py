import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools import generate_builtin_fonts


class GenerateBuiltinFontsTest(unittest.TestCase):
    def profile(self):
        return {
            "schema_version": 1,
            "profile": "builtin-latin-v1",
            "converter": "lv_font_conv@1.5.3",
            "bpp": 4,
            "ranges": [[32, 126], [160, 255], [65533, 65533]],
            "symbols": [0xF00B, 0xF011],
            "profiles": [
                {"role": "small", "size": 14},
                {"role": "medium", "size": 18},
                {"role": "large", "size": 24},
                {"role": "title", "size": 32},
            ],
        }

    def test_profile_has_exact_builtin_latin_v1_coverage(self):
        profile = self.profile()
        generate_builtin_fonts.validate_profile(profile)
        requested = generate_builtin_fonts.requested_codepoints(profile)
        self.assertEqual(len(requested), 194)
        self.assertIn(0x20, requested)
        self.assertIn(0xFF, requested)
        self.assertIn(0xFFFD, requested)
        self.assertIn(0xF00B, requested)
        self.assertNotIn(0x7F, requested)

    def test_compacts_ranges(self):
        self.assertEqual(
            generate_builtin_fonts.compact_ranges({32, 33, 34, 160, 161, 0xFFFD}),
            "0x20-0x22,0xa0-0xa1,0xfffd",
        )

    def test_sanitizes_non_reproducible_converter_command(self):
        source = "header\n * Opts: --font /private/path/font.ttf -o /tmp/output.c\nbody\n"
        sanitized = generate_builtin_fonts.sanitize_generated_source(
            source, "builtin-latin-v1", 14
        )
        self.assertNotIn("/private/path", sanitized)
        self.assertIn("Profile: builtin-latin-v1; size=14", sanitized)

    def test_rejects_invalid_profile(self):
        profile = self.profile()
        profile["profiles"][0]["role"] = "wrong"
        with self.assertRaises(ValueError):
            generate_builtin_fonts.validate_profile(profile)

    def test_finds_implicit_lvgl_widget_symbols(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            symbol_def = root / "lv_symbol_def.h"
            symbol_def.write_text(
                '#define LV_SYMBOL_OK "ok" /*61452, 0xF00C*/\n'
                '#define LV_SYMBOL_DOWN "down" /*61560, 0xF078*/\n',
                encoding="utf-8",
            )
            widget = root / "widget.c"
            widget.write_text("const char * symbol = LV_SYMBOL_DOWN;\n", encoding="utf-8")
            requirements = generate_builtin_fonts.lvgl_symbol_requirements(symbol_def, [widget])
            self.assertEqual(requirements, {"LV_SYMBOL_DOWN": 0xF078})
            with self.assertRaisesRegex(ValueError, "LV_SYMBOL_DOWN=U\\+F078"):
                generate_builtin_fonts.validate_lvgl_symbol_coverage(requirements, {0x20})

    def test_repository_profile_covers_host_and_default_widget_symbols(self):
        root = Path(__file__).resolve().parents[2]
        lvgl = root / "firmware/espressif/managed_components/lvgl__lvgl"
        requirements = generate_builtin_fonts.lvgl_symbol_requirements(
            lvgl / "src/font/lv_symbol_def.h",
            [
                root / "firmware/espressif/main/platform/lvgl",
                lvgl / "src/widgets/keyboard/lv_keyboard.c",
                lvgl / "src/widgets/dropdown/lv_dropdown.c",
            ],
        )
        profile = generate_builtin_fonts.load_json(
            root / "firmware/espressif/main/platform/lvgl/fonts/builtin-latin-v1.json"
        )
        generate_builtin_fonts.validate_lvgl_symbol_coverage(
            requirements, generate_builtin_fonts.requested_codepoints(profile)
        )


if __name__ == "__main__":
    unittest.main()
