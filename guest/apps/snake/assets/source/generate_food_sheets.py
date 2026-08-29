#!/usr/bin/env python3
"""Scale the Juicy Snake food sprite sheets without atlas-frame bleed."""

from __future__ import annotations

from pathlib import Path

from PIL import Image


SOURCE_DIR = Path(__file__).resolve().parent
ASSET_DIR = SOURCE_DIR.parent
FOOD_TYPES = ("normal", "golden", "poison", "speed")
SHEET_COLUMNS = 4
FRAME_COUNT = 16
SOURCE_FRAME_SIZE = 36
SCALE_NUMERATOR = 6
SCALE_DENOMINATOR = 5
OUTPUT_FRAME_SIZE = SOURCE_FRAME_SIZE * SCALE_NUMERATOR // SCALE_DENOMINATOR


def generate_sheet(food_type: str) -> None:
    source_path = SOURCE_DIR / f"snake_food_{food_type}_sheet_1x.png"
    output_path = ASSET_DIR / f"snake_food_{food_type}_sheet.png"
    expected_size = SOURCE_FRAME_SIZE * SHEET_COLUMNS

    with Image.open(source_path) as source:
        source = source.convert("RGBA")
        if source.size != (expected_size, expected_size):
            raise ValueError(f"{source_path} must be {expected_size}x{expected_size}, got {source.size}")

        output_size = OUTPUT_FRAME_SIZE * SHEET_COLUMNS
        output = Image.new("RGBA", (output_size, output_size), (0, 0, 0, 0))
        for frame_index in range(FRAME_COUNT):
            source_x = (frame_index % SHEET_COLUMNS) * SOURCE_FRAME_SIZE
            source_y = (frame_index // SHEET_COLUMNS) * SOURCE_FRAME_SIZE
            frame = source.crop(
                (source_x, source_y, source_x + SOURCE_FRAME_SIZE, source_y + SOURCE_FRAME_SIZE)
            )
            frame = frame.resize((OUTPUT_FRAME_SIZE, OUTPUT_FRAME_SIZE), Image.Resampling.LANCZOS)
            output_x = (frame_index % SHEET_COLUMNS) * OUTPUT_FRAME_SIZE
            output_y = (frame_index // SHEET_COLUMNS) * OUTPUT_FRAME_SIZE
            output.alpha_composite(frame, (output_x, output_y))

        output.save(output_path, optimize=True)


def main() -> None:
    for food_type in FOOD_TYPES:
        generate_sheet(food_type)


if __name__ == "__main__":
    main()
