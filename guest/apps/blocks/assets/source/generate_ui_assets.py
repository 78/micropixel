#!/usr/bin/env python3
"""Generate deterministic Juicy Blocks board and tile-strip resources."""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw


ASSET_DIR = Path(__file__).resolve().parent.parent
BACKGROUND = (5, 5, 5)
BOARD = (10, 10, 10)
GRID = (18, 18, 18)
BORDER = (38, 38, 38)
def generate_board() -> None:
    image = Image.new("RGB", (524, 600), BACKGROUND)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, 299, 599), radius=14, fill=BOARD, outline=BORDER, width=2)
    for column in range(1, 10):
        x = column * 30
        draw.line((x, 2, x, 597), fill=GRID, width=1)
    for row in range(1, 20):
        y = row * 30
        draw.line((2, y, 297, y), fill=GRID, width=1)

    panels = (
        (328, 0, 523, 126),
        (328, 142, 523, 268),
        (328, 284, 523, 384),
        (328, 400, 523, 500),
        (328, 516, 523, 599),
    )
    for panel in panels:
        draw.rounded_rectangle(panel, radius=12, fill=BOARD, outline=BORDER, width=2)
    image.save(ASSET_DIR / "blocks_board_524x600.png", optimize=True)


def main() -> None:
    generate_board()


if __name__ == "__main__":
    main()
