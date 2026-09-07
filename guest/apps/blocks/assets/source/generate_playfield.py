"""Generate static scene artwork using the original Blocks pixel geometry.

Run after changing kTetrominoColors. Pillow is required; no runtime pixel upload.
"""
from pathlib import Path
import re
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def mix(foreground, background, opacity):
    return tuple((f * opacity + b * (255 - opacity) + 127) // 255
                 for f, b in zip(foreground, background))


def inside(x, y, width, height, radius):
    if not (0 <= x < width and 0 <= y < height):
        return False
    if radius <= x < width - radius or radius <= y < height - radius:
        return True
    cx = radius if x < radius else width - radius - 1
    cy = radius if y < radius else height - radius - 1
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def generate():
    source = (ROOT / 'blocks_common.hpp').read_text().split('kTetrominoColors[] = {')[1].split('};')[0]
    colors = [tuple(map(int, match)) for match in re.findall(r'\{(\d+)U,\s*(\d+)U,\s*(\d+)U\}', source)]
    assert len(colors) == 7
    atlas = Image.new('RGBA', (240, 270))
    for variant in range(4):
        for index, piece in enumerate(colors):
            fill = mix(piece, (16, 16, 16), 56) if variant == 1 else piece
            if variant == 3:
                fill = mix(piece, (10, 10, 10), 96)
            highlight = mix((255, 255, 255), fill, 24 if variant == 1 else 72)
            for y in range(1, 29):
                for x in range(1, 29):
                    if variant >= 2:
                        if (3 <= x < 21 and 1 <= y < 23) or (1 <= x < 23 and 3 <= y < 21):
                            color = highlight if 5 <= x < 19 and 3 <= y < 5 else fill
                            atlas.putpixel((index * 30 + x, variant * 30 + y), (*color, 255))
                        continue
                    if not inside(x - 1, y - 1, 28, 28, 4):
                        continue
                    if variant == 1 and 4 <= x <= 25 and 4 <= y <= 25:
                        continue
                    color = highlight if y == 3 and 5 <= x <= 24 else fill
                    atlas.putpixel((index * 30 + x, variant * 30 + y), (*color, 255))
    theme_source = (ROOT / 'blocks_common.hpp').read_text().split('kThemes[] = {')[1].split('};')[0]
    accents = [tuple(map(int, match)) for match in re.findall(r'\{\{(\d+)U,\s*(\d+)U,\s*(\d+)U\}', theme_source)]
    assert len(accents) == 5
    for theme, accent in enumerate(accents):
        for phase in range(8):
            color = mix((255, 255, 255), accent, 80 + phase * 22)
            for y in range(1, 29):
                for x in range(1, 29):
                    atlas.putpixel((phase * 30 + x, (4 + theme) * 30 + y), (*color, 255))
    atlas.save(ROOT / 'assets/source/playfield_atlas.png')
    # Match one physical pixel at both the 720 and 480 logical-scale profiles.
    for stroke in (1, 2):
        board = Image.new('RGB', (300, 600), (5, 5, 5))
        for y in range(600):
            for x in range(300):
                if inside(x, y, 300, 600, 14):
                    inner = inside(x - 2, y - 2, 296, 596, 12)
                    color = (16, 16, 16) if inner else (38, 38, 38)
                    if inner and ((x != 0 and x % 30 < stroke) or (y != 0 and y % 30 < stroke)):
                        color = (32, 32, 32)
                    board.putpixel((x, y), color)
        board.save(ROOT / f'assets/source/playfield_background_{stroke}.png')


if __name__ == '__main__':
    generate()
