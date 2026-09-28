#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build an offline UI font component from a Noto CJK source.

The Host renders a non-English locale only from an installed font component
(`host/fonts/language_packs.cpp`), and the signed packs in the release pipeline
come from the Control API. A board that must show Chinese without a network or a
Control service instead gets a component seeded into the App Store image that is
flashed with the firmware; `LanguagePacks::Cached()` scans that same system
store, so a seeded pack is indistinguishable from a downloaded one.

This tool produces such a component. It deliberately does not touch
`build_language_fonts.py`, which stays the single source for the published packs:
that one is pinned to the DeepSeek tokenizer repertoire and to Unicode 16 for
reproducibility, while this one keeps only what the UI itself needs - the
characters of the localized catalogs, ASCII, and a published legacy repertoire -
so the pack stays small enough to sit in a flash image.

Inputs are explicit and hashed into the manifest; nothing is discovered and
nothing is fetched. See docs/development/language-fonts.zh-CN.md.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

# The bundle tool validates the emitted font exactly the way the packaging step
# will, so a generated component cannot fail validation only later.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_app_bundle import validate_static_ttf  # noqa: E402

# Published legacy character repertoires, same definition the release generator
# uses, deliberately smaller than every representable character.
REPERTOIRES = {
    "gb2312-level1": ("GB2312 level 1", "gb2312", [(range(0xB0, 0xD8), range(0xA1, 0xFF))]),
    "big5-level1": (
        "Big5 level 1",
        "big5",
        [(range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))],
    ),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def repertoire_charset(name):
    _, encoding, blocks = REPERTOIRES[name]
    result = set(range(32, 127))
    for leads, trails in blocks:
        for lead in leads:
            for trail in trails:
                if name == "big5-level1" and lead == 0xC6 and trail > 0x7E:
                    continue
                try:
                    result.update(map(ord, bytes([lead, trail]).decode(encoding)))
                except UnicodeDecodeError:
                    pass
    return result


def catalog_charset(catalogs, locale):
    """Every character the UI can print for this locale.

    The English catalog is included because the Host falls back to it for keys a
    translation has not covered yet, and a missing glyph renders as a
    placeholder rather than as the fallback text.
    """
    result = set()
    for name in (locale, "en"):
        path = catalogs / f"{name}.json"
        if not path.is_file():
            continue
        strings = json.loads(path.read_text(encoding="utf-8"))
        result.update(ord(ch) for value in strings.values() for ch in value if ord(ch) >= 32)
    return result


def required_charset(catalogs, locale):
    """Every codepoint the pack has to carry for this locale: catalogs plus ASCII."""
    return catalog_charset(catalogs, locale) | set(range(32, 127))


def instance_static(font, weight):
    """Return a static font, instancing a variable source at `weight`."""
    if "fvar" not in font:
        return font
    axes = {axis.axisTag: (axis.minValue, axis.defaultValue, axis.maxValue) for axis in font["fvar"].axes}
    if "wght" not in axes:
        raise ValueError("variable source has no weight axis")
    if not axes["wght"][0] <= weight <= axes["wght"][2]:
        raise ValueError(f"weight {weight} is outside the source range")
    from fontTools.varLib import instancer

    return instancer.instantiateVariableFont(font, {"wght": weight}, inplace=False, updateFontNames=True)


def component_documents(locale, version, repertoire):
    """Return the component id, project manifest and asset manifest."""
    identifier = f"micropixel.fonts.ui.{locale.lower()}"
    project = {
        "schema_version": 1,
        "package_type": "component",
        "component_type": "font",
        "id": identifier,
        "version": version,
        "title": {"default": "en", "values": {"en": f"UI font {locale}"}},
        "languages": [locale],
        "font_bundle": "noto-ui-subset-v1",
        "charset": f"ui-{repertoire}-v1",
        "font": {"asset": "regular", "format": "ttf"},
        "asset_manifest": "assets.json",
    }
    assets = {
        "schema_version": 1,
        "assets": [{"name": "regular", "format": "font_ttf", "path": "regular.ttf"}],
    }
    return identifier, project, assets


def build(source, locale, repertoire, catalogs, output, version, weight):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    source_bytes = source.read_bytes()
    required = required_charset(catalogs, locale)
    basic = repertoire_charset(repertoire)

    variable = TTFont(source, recalcTimestamp=False)
    font = instance_static(variable, weight)
    available = set(font.getBestCmap())
    missing = required - available
    if missing:
        raise ValueError(f"{locale}: UI glyphs absent from source: {sorted(missing)}")
    keep = (basic | required) & available
    if not keep:
        raise ValueError("source covers none of the requested repertoire")

    options = subset.Options()
    options.name_IDs = ["*"]  # Preserve copyright and the OFL notice in the artifact.
    options.hinting = False
    options.layout_features = ["*"]
    options.recalc_timestamp = False
    worker = subset.Subsetter(options=options)
    worker.populate(unicodes=keep)
    worker.subset(font)

    component = output / locale
    component.mkdir(parents=True, exist_ok=True)
    target = component / "regular.ttf"
    font.save(target)
    content = target.read_bytes()
    validate_static_ttf(content)
    sha = digest(content)
    (output / f"{sha}.ttf").write_bytes(content)
    for license_path in (Path(__file__).parent / "licenses").glob("*.txt"):
        (component / license_path.name).write_bytes(license_path.read_bytes())

    identifier, project, assets = component_documents(locale, version, repertoire)
    (component / "app.json").write_text(json.dumps(project, indent=2) + "\n", encoding="utf-8")
    (component / "assets.json").write_text(json.dumps(assets, indent=2) + "\n", encoding="utf-8")
    manifest = {
        "version": 1,
        "charset": f"ui-{repertoire}-v1",
        "locale": locale,
        "weight": weight,
        "source": source.name,
        "source_sha256": digest(source_bytes),
        "source_bytes": len(source_bytes),
        "bytes": len(content),
        "sha256": sha,
        "component_id": identifier,
        "codepoints": len(keep),
        "repertoire": REPERTOIRES[repertoire][0],
        "repertoire_codepoints": len(basic),
        "repertoire_missing_from_source": sorted(basic - available),
        "ui_codepoints": len(required),
    }
    (output / f"manifest-{locale}.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="Noto CJK TTF, static or variable")
    parser.add_argument("--locale", required=True, help="canonical Locale tag, e.g. zh-CN")
    parser.add_argument("--repertoire", default="gb2312-level1", choices=sorted(REPERTOIRES))
    parser.add_argument("--catalogs", type=Path, default=Path("firmware/espressif/main/host/ui/i18n"))
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--weight", type=int, default=400)
    args = parser.parse_args()
    print(
        json.dumps(
            build(args.source, args.locale, args.repertoire, args.catalogs, args.output, args.version, args.weight),
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
