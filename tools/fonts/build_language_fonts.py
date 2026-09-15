#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build immutable, unhinted Noto packs from a pinned DeepSeek tokenizer.

Input files are explicit. Neither a model download nor font discovery occurs as
part of a firmware build. See docs/development/language-fonts.zh-CN.md.
"""
import argparse
import hashlib
import json
import unicodedata
from pathlib import Path


DEEPSEEK_SHA256 = '8f9f37ca37fdc4f5fd36d5cf4d3b0e8392edb4e894fd10cc0d70b4957c8633cf'
SOURCES = {
    'en': ('NotoSans', 'fe8c022f48d8dd29f17b744d16f9346f4357e16f7d4f7be58b000ae7c291b614'),
    'zh-CN': ('NotoSansSC', 'dc71173babc38dfd019912965f2b4b3421fb347ebd854e7b96f64ad63673924a'),
    'zh-TW': ('NotoSansTC', 'e822851bd9dcfdc95d4eba4d9f513b23a13a50ae0d556ed4bfed7b14a30e7e5b'),
    'ja-JP': ('NotoSansJP', 'f32915916d2d6f35bb0d3d7ef0784504914bf0235afb02eec552bd14d06d3552'),
    'ko-KR': ('NotoSansKR', 'd120ae7d44307155ab593bbbe63e0bb7f5766e00492015e5d69f220092589156'),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tokenizer_charset(tokenizer):
    values = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    encoded = values.copy()
    for value in range(256):
        if value not in values:
            encoded.append(256 + sum(v >= 256 for v in encoded))
            values.append(value)
    inverse = dict(zip(map(chr, encoded), values))
    result = set()
    for token in tokenizer['model']['vocab']:
        if token.startswith('<｜'):
            continue
        try:
            text = bytes(inverse[ch] for ch in token).decode('utf-8', errors='ignore')
        except KeyError as error:
            raise ValueError('Unexpected non-byte BPE token') from error
        result.update(ord(ch) for ch in text if ord(ch) >= 32 and unicodedata.category(ch) not in ('Cc', 'Cs', 'Cn'))
    return result


# Published first-level legacy character repertoires, deliberately smaller than
# every representable character. UI extraction remains the coverage contract.
BASIC_REPERTOIRES = {
    'en': ('ASCII', 'ascii', []),
    'zh-CN': ('GB2312 level 1', 'gb2312', [(range(0xb0, 0xd8), range(0xa1, 0xff))]),
    'zh-TW': ('Big5 level 1', 'big5', [(range(0xa4, 0xc7), list(range(0x40, 0x7f)) + list(range(0xa1, 0xff)))]),
    'ja-JP': ('JIS X 0208 level 1', 'euc_jp', [(range(0xb0, 0xd0), range(0xa1, 0xff))]),
    'ko-KR': ('KS X 1001 Hangul', 'euc_kr', [(range(0xb0, 0xc9), range(0xa1, 0xff))]),
}

def basic_charset(locale):
    _, encoding, blocks = BASIC_REPERTOIRES[locale]
    result = set(range(32, 127))
    for leads, trails in blocks:
        for lead in leads:
            for trail in trails:
                # Big5 level 1 ends at C6 7E; subsequent codes are not level 1.
                if locale == 'zh-TW' and lead == 0xc6 and trail > 0x7e:
                    continue
                try:
                    result.update(map(ord, bytes([lead, trail]).decode(encoding)))
                except UnicodeDecodeError:
                    pass
    if locale == 'ja-JP':
        result |= set(range(0x3041, 0x3097)) | set(range(0x30a0, 0x3100))
    return result


def build(deepseek_path, sources, catalogs, output, version="1.0.0"):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    if unicodedata.unidata_version != "16.0.0":
        raise ValueError("Reproducible charset extraction requires Unicode data 16.0.0")
    deepseek_bytes = deepseek_path.read_bytes()
    if digest(deepseek_bytes) != DEEPSEEK_SHA256:
        raise ValueError('DeepSeek tokenizer SHA-256 mismatch')
    deepseek = tokenizer_charset(json.loads(deepseek_bytes))
    core = deepseek
    output.mkdir(parents=True, exist_ok=True)
    for license_path in (Path(__file__).parent / 'licenses').glob('*.txt'):
        (output / license_path.name).write_bytes(license_path.read_bytes())
    packs = []
    for locale, (name, expected) in SOURCES.items():
        source = sources / f'{name}-Regular.ttf'
        original = source.read_bytes()
        if digest(original) != expected:
            raise ValueError(f'{source.name}: source SHA-256 mismatch')
        font = TTFont(source, recalcTimestamp=False)
        if 'glyf' not in font or 'fvar' in font:
            raise ValueError('Language packs require static TrueType outlines')
        strings = json.loads((catalogs / f'{locale}.json').read_text())
        required = {ord(ch) for value in strings.values() for ch in value if ord(ch) >= 32}
        required |= set(range(32, 127))
        available = set(font.getBestCmap())
        missing = required - available
        if missing:
            raise ValueError(f'{locale}: UI glyphs absent from source: {sorted(missing)}')
        basic = basic_charset(locale)
        keep = (core | basic | required) & available
        options = subset.Options()
        options.name_IDs = ['*']  # Preserve copyright and OFL license in the published TTF.
        options.hinting = False
        options.layout_features = ['*']
        options.recalc_timestamp = False
        worker = subset.Subsetter(options=options)
        worker.populate(unicodes=keep)
        worker.subset(font)
        target = output / f'{locale}.ttf'
        font.save(target)
        content = target.read_bytes()
        sha = digest(content)
        target.rename(output / f'{sha}.ttf')
        component = output / locale
        component.mkdir(exist_ok=True)
        (component / 'regular.ttf').write_bytes(content)
        project = dict(schema_version=1, package_type='component', component_type='font',
                       id=f'micropixel.fonts.noto.{locale.lower()}', version=version,
                       title={'default': 'en', 'values': {'en': f'Noto {locale}'}},
                       languages=[locale], font_bundle='noto-v1', charset='deepseek-basic-v1',
                       font={'asset': 'regular', 'format': 'ttf'}, asset_manifest='assets.json')
        (component / 'app.json').write_text(json.dumps(project, indent=2) + '\n')
        (component / 'assets.json').write_text(json.dumps({'schema_version': 1, 'assets': [{'name': 'regular', 'format': 'font_ttf', 'path': 'regular.ttf'}]}, indent=2) + '\n')
        for license_path in (Path(__file__).parent / 'licenses').glob('*.txt'):
            (component / license_path.name).write_bytes(license_path.read_bytes())
        packs.append(dict(locale=locale, source=source.name, source_sha256=expected,
                          source_bytes=len(original), bytes=len(content), sha256=sha,
                          codepoints=len(keep), basic_repertoire=BASIC_REPERTOIRES[locale][0],
                          basic_codepoints=len(basic), basic_missing_from_source=sorted(basic - available),
                          ui_extra_codepoints=sorted(required - (core | basic))))
    manifest = dict(version=1, charset="deepseek-basic-v1",
                    deepseek_model="deepseek-ai/DeepSeek-V4-Flash", deepseek_sha256=DEEPSEEK_SHA256,
                    deepseek_codepoints=len(deepseek), packs=packs)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deepseek-tokenizer', required=True, type=Path)
    parser.add_argument('--sources', required=True, type=Path)
    parser.add_argument('--catalogs', type=Path, default=Path('firmware/espressif/main/host/ui/i18n'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', default='1.0.0')
    args = parser.parse_args()
    print(json.dumps(build(args.deepseek_tokenizer, args.sources, args.catalogs, args.output, args.version), indent=2))


if __name__ == '__main__':
    main()
