# Built-in font sources

These font inputs and their licenses are tracked in Git. A complete checkout contains everything
needed by the built-in font generator; no system fonts or LVGL archive extras are required.

| Source | Use | License |
| --- | --- | --- |
| [Montserrat-Medium.ttf](Montserrat-Medium.ttf) | Latin text and the embedded Tiny TTF font | [SIL OFL 1.1](LICENSE.Montserrat.txt) |
| [DejaVuSans.ttf](DejaVuSans.ttf) | U+FFFD replacement glyph | [Bitstream Vera / DejaVu](LICENSE.DejaVuSans.txt) |
| [FontAwesome5-Solid+Brands+Regular.woff](FontAwesome5-Solid+Brands+Regular.woff) | Built-in symbols and status icon generation | [Font Awesome Free](LICENSE.FontAwesome5.txt) |

Host builds compile the [prebuilt C sources](../builtin/) checked into this repository.
[The generator's](../../../../../../../tools/generate_builtin_fonts.py) `--check` mode validates
their input fingerprints and glyph coverage with Python alone; normal builds need no Node.js or npm.
The coverage report stays under the selected Host build directory's `esp-idf/main/generated/fonts/`.

After changing a font input or [builtin-latin-v1.json](../builtin-latin-v1.json), install Node.js and
npm, including `npx`, and run `cmake --build <configured-host-build> --target micropixel_regenerate_builtin_fonts`.
This explicit target uses pinned `lv_font_conv@1.5.3` and updates all nine checked-in C files.
Its first run needs network access or a prepared npm cache. Commit the regenerated files with their
inputs; do not edit the glyph arrays by hand.

See the [build guide](../../../../../../../docs/development/flashing.zh-CN.md) and
[language font guide](../../../../../../../docs/development/language-fonts.zh-CN.md) (Chinese).
Preserve the accompanying licenses when redistributing these sources or their generated fonts;
attribution is recorded in [third-party notices](../../../../../../../THIRD_PARTY_NOTICES.md#system-font-sources).
