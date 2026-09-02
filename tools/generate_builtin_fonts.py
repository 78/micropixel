#!/usr/bin/env python3
"""Generate MicroPixel's reproducible builtin-latin-v1 LVGL fonts."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path


GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6})(?: |\*)")
OPTS_COMMENT = re.compile(r" \* Opts:.*\n")
LVGL_SYMBOL_DEFINITION = re.compile(r"^\s*#define\s+(LV_SYMBOL_[A-Z0-9_]+).*?/\*(\d+),", re.MULTILINE)
LVGL_SYMBOL_USAGE = re.compile(r"\bLV_SYMBOL_[A-Z0-9_]+\b")
SDK_SYMBOL_DEFINITION = re.compile(
    r'^\s*inline constexpr char\s+(k[A-Za-z0-9_]+)\[\]\s*=\s*"((?:\\x[0-9A-Fa-f]{2})+)";'
    r"\s*//\s*U\+([0-9A-Fa-f]{4,6})",
    re.MULTILINE,
)
NON_GLYPH_LVGL_SYMBOLS = {"LV_SYMBOL_DUMMY"}


def load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"unable to read {path}: {error}") from error


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def requested_codepoints(profile: dict) -> set[int]:
    ranges = profile.get("ranges")
    if not isinstance(ranges, list) or not ranges:
        raise ValueError("font profile ranges must be a non-empty list")
    result: set[int] = set()
    for value in ranges:
        if (
            not isinstance(value, list)
            or len(value) != 2
            or not all(isinstance(item, int) for item in value)
            or value[0] < 0x20
            or value[0] > value[1]
            or value[1] > 0x10FFFF
            or value[0] <= 0xDFFF and value[1] >= 0xD800
        ):
            raise ValueError(f"invalid font codepoint range: {value!r}")
        result.update(range(value[0], value[1] + 1))
    symbols = profile.get("symbols", [])
    if (
        not isinstance(symbols, list)
        or not all(isinstance(value, int) and 0x20 <= value <= 0x10FFFF for value in symbols)
        or len(symbols) != len(set(symbols))
    ):
        raise ValueError("font profile symbols must be unique Unicode codepoints")
    result.update(symbols)
    return result


def compact_ranges(codepoints: set[int]) -> str:
    values = sorted(codepoints)
    ranges: list[str] = []
    start = previous = values[0]
    for value in values[1:]:
        if value == previous + 1:
            previous = value
            continue
        ranges.append(hex(start) if start == previous else f"{hex(start)}-{hex(previous)}")
        start = previous = value
    ranges.append(hex(start) if start == previous else f"{hex(start)}-{hex(previous)}")
    return ",".join(ranges)


def validate_profile(profile: dict) -> list[dict]:
    if profile.get("schema_version") != 1 or profile.get("profile") != "builtin-latin-v1":
        raise ValueError("font profile must be builtin-latin-v1 schema version 1")
    if profile.get("converter") != "lv_font_conv@1.5.3" or profile.get("bpp") not in (1, 2, 3, 4, 8):
        raise ValueError("font profile converter or bpp is unsupported")
    profiles = profile.get("profiles")
    expected_roles = ["small", "medium", "large", "title"]
    if not isinstance(profiles, list) or [value.get("role") for value in profiles] != expected_roles:
        raise ValueError("font profiles must define small, medium, large and title in order")
    for value in profiles:
        if not isinstance(value.get("size"), int) or value["size"] <= 0:
            raise ValueError(f"invalid font profile: {value!r}")
    supplemental_sizes = profile.get("supplemental_sizes", [])
    if (
        not isinstance(supplemental_sizes, list)
        or not all(isinstance(size, int) and size > 0 for size in supplemental_sizes)
        or len(supplemental_sizes) != len(set(supplemental_sizes))
        or set(supplemental_sizes) & {value["size"] for value in profiles}
    ):
        raise ValueError("supplemental font sizes must be unique positive sizes outside the semantic profile")
    return profiles


def validate_host_catalog_coverage(path: Path, requested: set[int]) -> dict:
    report = load_json(path)
    catalog_codepoints = report.get("requested_codepoints")
    if not isinstance(catalog_codepoints, list) or not all(isinstance(value, int) for value in catalog_codepoints):
        raise ValueError("Host localization report has no requested_codepoints list")
    missing = set(catalog_codepoints) - requested
    if missing:
        values = ", ".join(f"U+{value:04X}" for value in sorted(missing))
        raise ValueError(f"builtin-latin-v1 does not cover Host catalog characters: {values}")
    return report


def lvgl_symbol_requirements(symbol_def: Path, sources: list[Path]) -> dict[str, int]:
    definitions = {
        name: int(codepoint)
        for name, codepoint in LVGL_SYMBOL_DEFINITION.findall(symbol_def.read_text(encoding="utf-8"))
    }
    names: set[str] = set()
    for source in sources:
        paths = source.rglob("*") if source.is_dir() else [source]
        for path in paths:
            if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                names.update(LVGL_SYMBOL_USAGE.findall(path.read_text(encoding="utf-8")))
    unknown = names - definitions.keys() - NON_GLYPH_LVGL_SYMBOLS
    if unknown:
        raise ValueError(f"LVGL symbol definitions are unavailable: {', '.join(sorted(unknown))}")
    return {name: definitions[name] for name in sorted(names - NON_GLYPH_LVGL_SYMBOLS)}


def validate_lvgl_symbol_coverage(requirements: dict[str, int], requested: set[int]) -> None:
    missing = {name: codepoint for name, codepoint in requirements.items() if codepoint not in requested}
    if missing:
        values = ", ".join(f"{name}=U+{codepoint:04X}" for name, codepoint in missing.items())
        raise ValueError(f"builtin-latin-v1 does not cover required LVGL symbols: {values}")


def sdk_symbol_requirements(source: Path) -> dict[str, int]:
    requirements = {}
    for name, encoded, documented_codepoint in SDK_SYMBOL_DEFINITION.findall(source.read_text(encoding="utf-8")):
        raw = bytes(int(value, 16) for value in re.findall(r"\\x([0-9A-Fa-f]{2})", encoded))
        try:
            decoded = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError(f"Guest SDK symbol {name} is not valid UTF-8") from error
        codepoint = int(documented_codepoint, 16)
        if len(decoded) != 1 or ord(decoded) != codepoint:
            raise ValueError(f"Guest SDK symbol {name} UTF-8 does not match U+{codepoint:04X}")
        requirements[name] = codepoint
    if not requirements:
        raise ValueError("Guest SDK symbol source contains no stable symbol definitions")
    if len(requirements) != len(set(requirements.values())):
        raise ValueError("Guest SDK symbol source reuses a symbol codepoint")
    return requirements


def validate_sdk_symbol_coverage(requirements: dict[str, int], requested: set[int]) -> None:
    missing = {name: codepoint for name, codepoint in requirements.items() if codepoint not in requested}
    if missing:
        values = ", ".join(f"{name}=U+{codepoint:04X}" for name, codepoint in missing.items())
        raise ValueError(f"builtin-latin-v1 does not cover Guest SDK symbols: {values}")


def sanitize_generated_source(source: str, profile_name: str, size: int) -> str:
    replacement = f" * Profile: {profile_name}; size={size}; converter=lv_font_conv@1.5.3\n"
    source, count = OPTS_COMMENT.subn(replacement, source, count=1)
    if count != 1:
        raise ValueError("generated LVGL font has no converter options comment")
    return source


def run_converter(
    montserrat: Path,
    replacement_font: Path,
    symbol_font: Path,
    output: Path,
    size: int,
    bpp: int,
    requested: set[int],
    symbols: set[int],
) -> set[int]:
    latin = requested - symbols - {0xFFFD}
    symbol = f"font_builtin_latin_{size}"
    with tempfile.TemporaryDirectory() as temporary:
        temporary_output = Path(temporary) / output.name
        command = [
            "npx",
            "--yes",
            "lv_font_conv@1.5.3",
            "--no-compress",
            "--no-prefilter",
            "--force-fast-kern-format",
            "--font",
            str(montserrat),
            "-r",
            compact_ranges(latin),
            "--font",
            str(replacement_font),
            "-r",
            "0xfffd",
            "--font",
            str(symbol_font),
            "-r",
            compact_ranges(symbols),
            "--format",
            "lvgl",
            "--bpp",
            str(bpp),
            "--size",
            str(size),
            "--lv-include",
            "lvgl.h",
            "--lv-font-name",
            symbol,
            "-o",
            str(temporary_output),
        ]
        subprocess.run(command, check=True)
        generated = temporary_output.read_text(encoding="utf-8")
    generated = sanitize_generated_source(generated, "builtin-latin-v1", size)
    covered = {int(match.group(1), 16) for match in GLYPH_COMMENT.finditer(generated)}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generated, encoding="utf-8")
    return covered


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--host-catalog-report", type=Path, required=True)
    parser.add_argument("--montserrat", type=Path, required=True)
    parser.add_argument("--replacement-font", type=Path, required=True)
    parser.add_argument("--symbol-font", type=Path, required=True)
    parser.add_argument("--lvgl-symbol-def", type=Path, required=True)
    parser.add_argument("--lvgl-symbol-source", type=Path, action="append", required=True)
    parser.add_argument("--sdk-symbol-source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    try:
        profile = load_json(args.profile)
        profiles = validate_profile(profile)
        requested = requested_codepoints(profile)
        symbols = set(profile.get("symbols", []))
        host_report = validate_host_catalog_coverage(args.host_catalog_report, requested)
        lvgl_symbols = lvgl_symbol_requirements(args.lvgl_symbol_def, args.lvgl_symbol_source)
        validate_lvgl_symbol_coverage(lvgl_symbols, requested)
        sdk_symbols = sdk_symbol_requirements(args.sdk_symbol_source)
        validate_sdk_symbol_coverage(sdk_symbols, requested)
        if not args.montserrat.is_file() or not args.replacement_font.is_file() or not args.symbol_font.is_file():
            raise ValueError("LVGL managed-component font sources are unavailable")

        variants = [
            *profiles,
            *(
                {"variant": "supplemental", "size": size}
                for size in profile.get("supplemental_sizes", [])
            ),
        ]
        profile_reports = []
        covered_intersection: set[int] | None = None
        for value in variants:
            size = value["size"]
            output = args.output_dir / f"font_builtin_latin_{size}.c"
            covered = run_converter(
                args.montserrat,
                args.replacement_font,
                args.symbol_font,
                output,
                size,
                profile["bpp"],
                requested,
                symbols,
            )
            missing = requested - covered
            if missing:
                values = ", ".join(f"U+{codepoint:04X}" for codepoint in sorted(missing))
                raise ValueError(f"generated {size}px font is missing requested glyphs: {values}")
            covered_intersection = covered if covered_intersection is None else covered_intersection & covered
            profile_reports.append(
                {
                    **value,
                    "bpp": profile["bpp"],
                    "file": output.name,
                    "bytes": output.stat().st_size,
                    "sha256": file_sha256(output),
                }
            )
        assert covered_intersection is not None
        missing = sorted(requested - covered_intersection)
        report = {
            "schema_version": 1,
            "profile": profile["profile"],
            "converter": profile["converter"],
            "sources": {
                "montserrat": {"file": args.montserrat.name, "sha256": file_sha256(args.montserrat)},
                "replacement": {
                    "file": args.replacement_font.name,
                    "sha256": file_sha256(args.replacement_font),
                },
                "symbols": {"file": args.symbol_font.name, "sha256": file_sha256(args.symbol_font)},
            },
            "host_catalog_fingerprint": host_report.get("fingerprint"),
            "required_lvgl_symbols": lvgl_symbols,
            "required_sdk_symbols": sdk_symbols,
            "requested_count": len(requested),
            "requested_codepoints": sorted(requested),
            "covered_count": len(covered_intersection),
            "covered_codepoints": sorted(covered_intersection),
            "missing_count": len(missing),
            "missing_codepoints": missing,
            "profiles": profile_reports,
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))

    print(
        f"Generated {len(variants)} {profile['profile']} fonts: "
        f"requested={len(requested)} covered={len(covered_intersection)} missing={len(missing)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
