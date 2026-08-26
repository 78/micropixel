#!/usr/bin/env python3
"""Validate JSON translations and generate a fixed, allocation-free C++ catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path


LOCALE_MAX_UTF8_BYTES = 31
KEY_PATTERN = re.compile(r"[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*")
NAMESPACE_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass(frozen=True)
class CatalogInput:
    default_locale: str
    catalogs: dict[str, Path]
    guest_api: bool


def normalize_locale_tag(value: object) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > LOCALE_MAX_UTF8_BYTES:
        raise ValueError("Locale tag must be a non-empty ASCII string of at most 31 bytes")
    if value.startswith("-") or value.endswith("-") or "--" in value:
        raise ValueError(f"invalid Locale tag: {value!r}")
    subtags = value.split("-")
    if not 2 <= len(subtags[0]) <= 3 or not subtags[0].isascii() or not subtags[0].isalpha():
        raise ValueError(f"invalid Locale language subtag: {value!r}")
    normalized = [subtags[0].lower()]
    has_script = False
    has_region = False
    for subtag in subtags[1:]:
        if (
            not has_script
            and not has_region
            and len(subtag) == 4
            and subtag.isascii()
            and subtag.isalpha()
        ):
            normalized.append(subtag.title())
            has_script = True
            continue
        is_region = (
            len(subtag) == 2 and subtag.isascii() and subtag.isalpha()
        ) or (
            len(subtag) == 3 and subtag.isascii() and subtag.isdigit()
        )
        if not has_region and is_region:
            normalized.append(subtag.upper() if subtag.isalpha() else subtag)
            has_region = True
            continue
        raise ValueError(f"unsupported Locale subtag in {value!r}")
    return "-".join(normalized)


def read_json_object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"unable to read JSON object {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def catalog_from_manifest(path: Path) -> CatalogInput:
    manifest = read_json_object(path)
    localization = manifest.get("localization")
    if not isinstance(localization, dict):
        raise ValueError("app manifest requires a localization object")
    default_locale = normalize_locale_tag(localization.get("default"))
    translations = localization.get("translations")
    if not isinstance(translations, dict) or not translations:
        raise ValueError("localization.translations must be a non-empty object")

    app_root = path.resolve().parent
    catalogs: dict[str, Path] = {}
    for raw_locale, raw_path in sorted(translations.items()):
        locale = normalize_locale_tag(raw_locale)
        if raw_locale != locale:
            raise ValueError(f"Locale key must use canonical spelling: {raw_locale!r} -> {locale!r}")
        if locale in catalogs:
            raise ValueError(f"duplicate normalized Locale: {locale}")
        if not isinstance(raw_path, str) or not raw_path or Path(raw_path).is_absolute():
            raise ValueError(f"translation path for {locale} must be relative to the app manifest")
        catalog_path = (app_root / raw_path).resolve()
        if not catalog_path.is_relative_to(app_root):
            raise ValueError(f"translation path escapes the app directory: {raw_path}")
        catalogs[locale] = catalog_path
    if default_locale not in catalogs:
        raise ValueError(f"default Locale {default_locale!r} has no translation file")
    return CatalogInput(default_locale, catalogs, True)


def catalog_from_directory(path: Path, default_locale_value: object) -> CatalogInput:
    default_locale = normalize_locale_tag(default_locale_value)
    catalogs: dict[str, Path] = {}
    for catalog_path in sorted(path.glob("*.json")):
        locale = normalize_locale_tag(catalog_path.stem)
        if catalog_path.stem != locale:
            raise ValueError(
                f"Host catalog filename must use canonical Locale spelling: {catalog_path.name!r}"
            )
        catalogs[locale] = catalog_path.resolve()
    if not catalogs:
        raise ValueError(f"catalog directory contains no JSON files: {path}")
    if default_locale not in catalogs:
        raise ValueError(f"default Locale {default_locale!r} has no catalog file")
    return CatalogInput(default_locale, catalogs, False)


def validate_text(key: str, value: object, path: Path) -> str:
    if not isinstance(value, str):
        raise ValueError(f"translation {key!r} in {path} must be a string")
    if not value:
        raise ValueError(f"translation {key!r} in {path} must not be empty")
    for character in value:
        codepoint = ord(character)
        if codepoint == 0 or codepoint == 0x7F or codepoint < 0x20:
            raise ValueError(
                f"translation {key!r} in {path} contains unsupported control U+{codepoint:04X}"
            )
        if 0xD800 <= codepoint <= 0xDFFF:
            raise ValueError(f"translation {key!r} in {path} contains a surrogate codepoint")
    return value


def load_catalogs(source: CatalogInput) -> tuple[list[str], dict[str, dict[str, str]]]:
    loaded: dict[str, dict[str, str]] = {}
    expected_keys: list[str] | None = None
    for locale, path in source.catalogs.items():
        payload = read_json_object(path)
        catalog: dict[str, str] = {}
        for key, value in payload.items():
            if not isinstance(key, str) or KEY_PATTERN.fullmatch(key) is None:
                raise ValueError(f"invalid translation key {key!r} in {path}")
            catalog[key] = validate_text(key, value, path)
        keys = sorted(catalog)
        if not keys:
            raise ValueError(f"translation catalog is empty: {path}")
        if expected_keys is None:
            expected_keys = keys
        elif keys != expected_keys:
            missing = sorted(set(expected_keys) - set(keys))
            extra = sorted(set(keys) - set(expected_keys))
            raise ValueError(f"catalog keys differ for {locale}: missing={missing} extra={extra}")
        loaded[locale] = catalog
    assert expected_keys is not None
    return expected_keys, loaded


def enum_name(key: str) -> str:
    words = re.split(r"[._-]", key)
    return "k" + "".join(word[:1].upper() + word[1:] for word in words)


def cpp_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def symbol_name(locale: str) -> str:
    return "kStrings" + "".join(part[:1].upper() + part[1:] for part in locale.split("-"))


def generate_header(
    namespace: str,
    source: CatalogInput,
    keys: list[str],
    catalogs: dict[str, dict[str, str]],
    fingerprint: str,
) -> str:
    enum_names = [enum_name(key) for key in keys]
    if len(enum_names) != len(set(enum_names)):
        raise ValueError("translation keys collide after conversion to C++ enum names")

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/generate_localization.py; do not edit.",
        f"// Catalog fingerprint: {fingerprint}",
        "",
        "#include <array>",
        "#include <span>",
        "#include <string_view>",
        "#include <stdint.h>",
    ]
    if source.guest_api:
        lines += ["", '#include "sdk/localization.hpp"']
    lines += ["", f"namespace {namespace} {{", "", "enum class Id : uint16_t {"]
    lines += [f"    {name}," for name in enum_names]
    lines += ["};", "", f"inline constexpr size_t kStringCount = {len(keys)}U;", ""]

    for locale, catalog in catalogs.items():
        lines.append(f"inline constexpr std::array<std::string_view, kStringCount> {symbol_name(locale)}{{")
        lines += [f"    {cpp_string(catalog[key])}," for key in keys]
        lines += ["};", ""]

    lines += [
        "class Catalog final {",
        "   public:",
        "    [[nodiscard]] constexpr std::string_view View(Id id) const {",
        "        const size_t index = static_cast<size_t>(id);",
        "        return index < strings_.size() ? strings_[index] : std::string_view{};",
        "    }",
        "    [[nodiscard]] constexpr const char* Get(Id id) const { return View(id).data(); }",
        "",
        "   private:",
        "    explicit constexpr Catalog(std::span<const std::string_view> strings) : strings_(strings) {}",
        "    std::span<const std::string_view> strings_{};",
        "",
        "    friend Catalog ForTag(const char* tag);",
        "};",
        "",
        "namespace detail {",
        "",
        "struct Part final {",
        "    const char* data{};",
        "    size_t size{};",
        "};",
        "",
        "struct LocaleParts final {",
        "    Part language{};",
        "    Part script{};",
        "    Part region{};",
        "};",
        "",
        "inline bool Equal(Part left, Part right) {",
        "    if (left.size != right.size) return false;",
        "    for (size_t index = 0U; index < left.size; ++index) {",
        "        if (left.data[index] != right.data[index]) return false;",
        "    }",
        "    return true;",
        "}",
        "",
        "inline bool Equal(Part left, const char* right) {",
        "    size_t size = 0U;",
        "    while (right[size] != '\\0') ++size;",
        "    return Equal(left, Part{right, size});",
        "}",
        "",
        "inline bool Equal(const char* left, const char* right) {",
        "    size_t index = 0U;",
        "    while (left[index] != '\\0' && left[index] == right[index]) ++index;",
        "    return left[index] == right[index];",
        "}",
        "",
        "inline Part NextPart(const char*& cursor) {",
        "    const char* start = cursor;",
        "    while (*cursor != '\\0' && *cursor != '-') ++cursor;",
        "    const Part result{start, static_cast<size_t>(cursor - start)};",
        "    if (*cursor == '-') ++cursor;",
        "    return result;",
        "}",
        "",
        "inline LocaleParts ParseLocale(const char* tag) {",
        "    LocaleParts result{};",
        "    const char* cursor = tag != nullptr ? tag : \"\";",
        "    result.language = NextPart(cursor);",
        "    if (*cursor == '\\0') return result;",
        "    const Part second = NextPart(cursor);",
        "    if (second.size == 4U) result.script = second;",
        "    else result.region = second;",
        "    if (*cursor != '\\0' && result.script.size != 0U) result.region = NextPart(cursor);",
        "    return result;",
        "}",
        "",
        "inline Part LikelyScript(const LocaleParts& locale) {",
        "    if (!Equal(locale.language, \"zh\")) return locale.script;",
        "    if (locale.script.size != 0U) return locale.script;",
        "    if (Equal(locale.region, \"TW\") || Equal(locale.region, \"HK\") || Equal(locale.region, \"MO\"))",
        "        return Part{\"Hant\", 4U};",
        "    if (Equal(locale.region, \"CN\") || Equal(locale.region, \"SG\")) return Part{\"Hans\", 4U};",
        "    return Part{};",
        "}",
        "",
        "inline bool SameLanguageAndScript(const char* left, const char* right) {",
        "    const LocaleParts left_parts = ParseLocale(left);",
        "    const LocaleParts right_parts = ParseLocale(right);",
        "    const Part script = LikelyScript(left_parts);",
        "    return Equal(left_parts.language, right_parts.language) && script.size != 0U &&",
        "           Equal(script, LikelyScript(right_parts));",
        "}",
        "",
        "inline bool IsBareLanguageMatch(const char* requested, const char* available) {",
        "    const LocaleParts requested_parts = ParseLocale(requested);",
        "    const LocaleParts available_parts = ParseLocale(available);",
        "    return Equal(requested_parts.language, available_parts.language) && available_parts.script.size == 0U &&",
        "           available_parts.region.size == 0U;",
        "}",
        "",
        "}  // namespace detail",
        "",
        "inline Catalog ForTag(const char* tag) {",
    ]
    for locale in catalogs:
        lines.append(
            f"    if (detail::Equal(tag, {cpp_string(locale)})) "
            f"return Catalog{{std::span<const std::string_view>{{{symbol_name(locale)}}}}};"
        )
    for locale in catalogs:
        lines.append(
            f"    if (detail::SameLanguageAndScript(tag, {cpp_string(locale)})) "
            f"return Catalog{{std::span<const std::string_view>{{{symbol_name(locale)}}}}};"
        )
    for locale in catalogs:
        lines.append(
            f"    if (detail::IsBareLanguageMatch(tag, {cpp_string(locale)})) "
            f"return Catalog{{std::span<const std::string_view>{{{symbol_name(locale)}}}}};"
        )
    lines += [
        f"    return Catalog{{std::span<const std::string_view>{{{symbol_name(source.default_locale)}}}}};",
        "}",
    ]
    if source.guest_api:
        lines += [
            "",
            "inline Catalog ForLocale(const micropixel::Locale& locale) { return ForTag(locale.tag()); }",
        ]
    lines += ["", f"}}  // namespace {namespace}", ""]
    return "\n".join(lines)


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_report(
    source: CatalogInput, keys: list[str], catalogs: dict[str, dict[str, str]]
) -> tuple[dict, str]:
    digest = hashlib.sha256(b"MicroPixel localization catalog v1\0")
    digest.update(source.default_locale.encode("ascii"))
    digest.update(b"\0")
    for key in keys:
        digest.update(key.encode("utf-8"))
        digest.update(b"\0")
    locale_reports = {}
    requested_union: set[int] = set()
    for locale, catalog in catalogs.items():
        codepoints = sorted({ord(character) for value in catalog.values() for character in value})
        requested_union.update(codepoints)
        source_digest = file_sha256(source.catalogs[locale])
        digest.update(locale.encode("ascii"))
        digest.update(bytes.fromhex(source_digest))
        locale_reports[locale] = {
            "path": source.catalogs[locale].name,
            "sha256": source_digest,
            "requested_count": len(codepoints),
            "requested_codepoints": codepoints,
        }
    fingerprint = digest.hexdigest()
    return {
        "schema_version": 1,
        "default_locale": source.default_locale,
        "key_count": len(keys),
        "requested_count": len(requested_union),
        "requested_codepoints": sorted(requested_union),
        "catalogs": locale_reports,
        "fingerprint": fingerprint,
    }, fingerprint


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--app-manifest", type=Path)
    source.add_argument("--catalog-dir", type=Path)
    parser.add_argument("--default-locale", help="required with --catalog-dir")
    parser.add_argument("--cpp-namespace", required=True)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    if NAMESPACE_PATTERN.fullmatch(args.cpp_namespace) is None:
        parser.error("--cpp-namespace must be one C++ identifier")
    try:
        if args.app_manifest is not None:
            if args.default_locale is not None:
                parser.error("--default-locale is only valid with --catalog-dir")
            catalog_input = catalog_from_manifest(args.app_manifest)
        else:
            if args.default_locale is None:
                parser.error("--default-locale is required with --catalog-dir")
            catalog_input = catalog_from_directory(args.catalog_dir, args.default_locale)
        keys, catalogs = load_catalogs(catalog_input)
        report, fingerprint = make_report(catalog_input, keys, catalogs)
        header = generate_header(args.cpp_namespace, catalog_input, keys, catalogs, fingerprint)
    except ValueError as error:
        parser.error(str(error))

    args.output_header.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output_header.write_text(header, encoding="utf-8")
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(
        f"Generated localization catalog: locales={len(catalogs)} keys={len(keys)} "
        f"characters={report['requested_count']} fingerprint={fingerprint}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
