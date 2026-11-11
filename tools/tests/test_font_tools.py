import contextlib
import hashlib
import io
import json
import shutil
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest import mock

from tools import build_app_bundle, build_font_cbin, generate_builtin_fonts
from tools.fonts import build_ui_font_subset


class NpxLauncherTest(unittest.TestCase):
    def test_resolves_the_host_npx_executable(self) -> None:
        """The bare command name is not enough: Windows resolves npx.cmd."""

        with mock.patch.object(
            generate_builtin_fonts.shutil, "which", return_value="/usr/local/bin/npx"
        ):
            self.assertEqual(
                generate_builtin_fonts.npx_launcher(), "/usr/local/bin/npx"
            )

    def test_missing_node_reports_an_actionable_error(self) -> None:
        with mock.patch.object(generate_builtin_fonts.shutil, "which", return_value=None):
            with self.assertRaises(ValueError) as raised:
                generate_builtin_fonts.npx_launcher()
        self.assertIn("Node.js", str(raised.exception))


class BuildFontCbinTest(unittest.TestCase):
    def test_header_records_payload_profile_and_hashes(self) -> None:
        payload = bytes(range(64))
        charset = b"U+0020..U+007E\n"
        package = build_font_cbin.build_package(payload, "latin-fixture-v1", 18, charset)
        self.assertEqual(len(package), build_font_cbin.HEADER_SIZE + len(payload))
        fields = build_font_cbin.HEADER.unpack(package[: build_font_cbin.HEADER_SIZE])
        self.assertEqual(fields[0], build_font_cbin.MAGIC)
        self.assertEqual(fields[1:3], (build_font_cbin.HEADER_VERSION, build_font_cbin.HEADER_SIZE))
        self.assertEqual(fields[3:7], (len(package), build_font_cbin.HEADER_SIZE, len(payload), build_font_cbin.FORMAT_LVGL_CBIN_V1))
        self.assertEqual(fields[7:10], build_font_cbin.LVGL_VERSION)
        self.assertEqual(fields[10:13], (build_font_cbin.ENDIAN_LITTLE, build_font_cbin.POINTER_SIZE, build_font_cbin.GLYPH_DSC_LARGE))
        self.assertEqual(fields[14], 18)
        self.assertEqual(fields[16].rstrip(b"\0"), b"latin-fixture-v1")
        self.assertEqual(fields[17], hashlib.sha256(charset).digest())
        self.assertEqual(fields[18], hashlib.sha256(payload).digest())
        self.assertEqual(package[build_font_cbin.HEADER_SIZE :], payload)

    def test_rejects_invalid_profile_and_empty_payload(self) -> None:
        for profile in ("", "contains space", "x" * 32, "中文"):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                build_font_cbin.build_package(b"payload", profile, 18, b"charset")
        with self.assertRaises(ValueError):
            build_font_cbin.build_package(b"", "fixture-v1", 18, b"charset")
        with self.assertRaises(ValueError):
            build_font_cbin.build_package(b"payload", "fixture-v1", 0, b"charset")


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
            "supplemental_sizes": [10, 12, 16, 20, 26],
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

    def test_rejects_duplicate_supplemental_size(self):
        profile = self.profile()
        profile["supplemental_sizes"] = [12, 14]
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

    def test_finds_guest_sdk_symbols(self):
        with TemporaryDirectory() as temporary:
            source = Path(temporary) / "symbols.hpp"
            source.write_text(
                'inline constexpr char kLeft[] = "\\xEF\\x81\\x93"; // U+F053\n'
                'inline constexpr char kUp[] = "\\xEF\\x81\\xB7"; // U+F077\n',
                encoding="utf-8",
            )
            requirements = generate_builtin_fonts.sdk_symbol_requirements(source)
            self.assertEqual(requirements, {"kLeft": 0xF053, "kUp": 0xF077})
            with self.assertRaisesRegex(ValueError, "kUp=U\\+F077"):
                generate_builtin_fonts.validate_sdk_symbol_coverage(requirements, {0xF053})

    def test_repository_profile_covers_host_and_default_widget_symbols(self):
        root = Path(__file__).resolve().parents[2]
        lvgl = root / "firmware/espressif/managed_components/lvgl__lvgl"
        requirements = generate_builtin_fonts.lvgl_symbol_requirements(
            lvgl / "include/lvgl/font/lv_symbol_def.h",
            [
                root / "firmware/espressif/main/host/ui",
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
        sdk_requirements = generate_builtin_fonts.sdk_symbol_requirements(root / "guest/sdk/symbols.hpp")
        generate_builtin_fonts.validate_sdk_symbol_coverage(
            sdk_requirements, generate_builtin_fonts.requested_codepoints(profile)
        )


class PrebuiltFontsTest(unittest.TestCase):
    root = Path(__file__).resolve().parents[2]
    fonts = root / "firmware/espressif/main/platform/lvgl/fonts"

    def check(self, temporary: Path, output: Path, profile: Path | None = None):
        lvgl = self.root / "firmware/espressif/managed_components/lvgl__lvgl"
        catalog = temporary / "catalog.json"
        catalog.write_text(json.dumps({"requested_codepoints": [32, 65]}), encoding="utf-8")
        arguments = [
            "--check", "--profile", str(profile or self.fonts / "builtin-latin-v1.json"),
            "--host-catalog-report", str(catalog),
            "--montserrat", str(self.fonts / "vendor/Montserrat-Medium.ttf"),
            "--replacement-font", str(self.fonts / "vendor/DejaVuSans.ttf"),
            "--symbol-font", str(self.fonts / "vendor/FontAwesome5-Solid+Brands+Regular.woff"),
            "--lvgl-symbol-def", str(lvgl / "include/lvgl/font/lv_symbol_def.h"),
            "--lvgl-symbol-source", str(self.root / "firmware/espressif/main/host/ui"),
            "--lvgl-symbol-source", str(self.root / "firmware/espressif/main/platform/lvgl"),
            "--lvgl-symbol-source", str(lvgl / "src/widgets/keyboard/lv_keyboard.c"),
            "--lvgl-symbol-source", str(lvgl / "src/widgets/dropdown/lv_dropdown.c"),
            "--sdk-symbol-source", str(self.root / "guest/sdk/symbols.hpp"),
            "--output-dir", str(output), "--report", str(temporary / "report.json"),
        ]
        with mock.patch.object(generate_builtin_fonts, "npx_launcher", side_effect=AssertionError("npx invoked")):
            return generate_builtin_fonts.main(arguments)

    def test_checkout_fonts_validate_without_node(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(self.check(root, self.fonts / "builtin"), 0)
            report = json.loads((root / "report.json").read_text())
            self.assertEqual(len(report["profiles"]), 9)
            self.assertEqual(report["missing_count"], 0)

    def test_profile_change_requires_regeneration(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile = json.loads((self.fonts / "builtin-latin-v1.json").read_text())
            profile["bpp"] = 2
            changed = root / "changed.json"
            changed.write_text(json.dumps(profile), encoding="utf-8")
            error = io.StringIO()
            with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
                self.check(root, self.fonts / "builtin", changed)
            self.assertEqual(raised.exception.code, 2)
            self.assertIn("stale; run target micropixel_regenerate_builtin_fonts", error.getvalue())

    def test_missing_font_and_missing_glyph_are_rejected(self):
        for failure in ("file", "glyph"):
            with self.subTest(failure=failure), TemporaryDirectory() as temporary:
                root = Path(temporary)
                output = root / "fonts"
                output.mkdir()
                font = output / "font_builtin_latin_14.c"
                if failure == "glyph":
                    shutil.copyfile(self.fonts / "builtin" / font.name, font)
                    font.write_text(font.read_text().replace("/* U+0041 ", "/* removed "), encoding="utf-8")
                error = io.StringIO()
                with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as raised:
                    self.check(root, output)
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("missing" if failure == "file" else "U+0041", error.getvalue())


class UiFontSubsetTest(unittest.TestCase):
    """The offline UI pack: small enough to flash, still covering the catalogs."""

    catalogs = Path("firmware/espressif/main/host/ui/i18n")

    def test_gb2312_level1_repertoire_matches_the_published_table(self):
        charset = build_ui_font_subset.repertoire_charset("gb2312-level1")
        self.assertEqual(len(charset), 3850)
        self.assertEqual(sum(1 for value in charset if 0x4E00 <= value <= 0x9FFF), 3755)
        self.assertEqual(sorted(value for value in charset if value < 0x80), list(range(32, 127)))

    def test_chinese_catalog_stays_inside_the_measured_coverage(self):
        """Text outside this scope means the pack has to be regenerated."""

        required = build_ui_font_subset.required_charset(self.catalogs, "zh-CN")
        basic = build_ui_font_subset.repertoire_charset("gb2312-level1")
        self.assertEqual(len(required), 446)
        # The catalogs reach past GB2312 level 1 for a handful of characters.
        self.assertEqual(len(required - basic), 12)

    def test_component_documents_parse_as_a_ttf_font_component(self):
        identifier, project, assets = build_ui_font_subset.component_documents("zh-CN", "1.0.0", "gb2312-level1")
        self.assertEqual(identifier, "micropixel.fonts.ui.zh-cn")
        self.assertEqual(assets["assets"], [{"name": "regular", "format": "font_ttf", "path": "regular.ttf"}])
        with TemporaryDirectory() as temporary:
            manifest_path = Path(temporary) / "app.json"
            manifest_path.write_text(json.dumps(project), encoding="utf-8")
            manifest = build_app_bundle.load_package_manifest(manifest_path)
        self.assertEqual(manifest.package_type, "component")
        self.assertEqual(manifest.component_type, "font")
        self.assertEqual(manifest.languages, ("zh-CN",))
        self.assertEqual(manifest.ttf_asset, "regular")
        self.assertEqual(manifest.font_bundle, "noto-ui-subset-v1")

    def test_a_static_source_is_used_as_it_is(self):
        source = {}
        self.assertIs(build_ui_font_subset.instance_static(source, 400), source)

    def test_a_variable_source_without_a_weight_axis_is_rejected(self):
        source = {"fvar": SimpleNamespace(axes=[])}
        with self.assertRaises(ValueError):
            build_ui_font_subset.instance_static(source, 400)


if __name__ == "__main__":
    unittest.main()
