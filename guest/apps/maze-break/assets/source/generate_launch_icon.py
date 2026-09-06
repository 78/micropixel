#!/usr/bin/env python3
"""Render the Maze Break launch icon: a corridor seen through the raycaster.

360x360 RGBA PNG, drawn with a tiny raycaster over a hard-coded map so the
icon matches the in-game look without shipping hand-made art. Pure Python.

Usage:
    python3 generate_launch_icon.py [--output ../launch.png]
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

SIZE = 360
MAP = [
    "#########",
    "####%####",
    "###...###",
    "###...###",
    "###...###",
    "###...###",
    "###...###",
    "#...S...#",
    "#########",
]
COLOURS = {
    "#": (178, 112, 58),  # brick
    "%": (132, 150, 178),  # steel
}
FLOOR = (90, 90, 90)
CEILING = (70, 62, 50)


def shade(colour: tuple[int, int, int], factor: float) -> tuple[int, int, int]:
    factor = max(0.0, min(1.0, factor))
    return tuple(int(c * factor) for c in colour)  # type: ignore[return-value]


def render() -> bytes:
    px, py = 4.5, 7.5
    angle = -math.pi / 2  # face north into the corridor
    dir_x, dir_y = math.cos(angle), math.sin(angle)
    plane_x, plane_y = -dir_y * 0.66, dir_x * 0.66
    zbuffer = []
    for x in range(SIZE):
        camera = 2.0 * x / SIZE - 1.0
        ray_x = dir_x + plane_x * camera
        ray_y = dir_y + plane_y * camera
        map_x, map_y = int(px), int(py)
        delta_x = abs(1.0 / ray_x) if ray_x else 1e30
        delta_y = abs(1.0 / ray_y) if ray_y else 1e30
        step_x = -1 if ray_x < 0 else 1
        step_y = -1 if ray_y < 0 else 1
        side_x = (px - map_x) * delta_x if ray_x < 0 else (map_x + 1 - px) * delta_x
        side_y = (py - map_y) * delta_y if ray_y < 0 else (map_y + 1 - py) * delta_y
        side = 0
        while True:
            if side_x < side_y:
                side_x += delta_x
                map_x += step_x
                side = 0
            else:
                side_y += delta_y
                map_y += step_y
                side = 1
            if MAP[map_y][map_x] in COLOURS:
                break
        dist = (side_x - delta_x) if side == 0 else (side_y - delta_y)
        wall_u = (py + dist * ray_y) if side == 0 else (px + dist * ray_x)
        zbuffer.append((dist, side, COLOURS[MAP[map_y][map_x]], wall_u % 1.0))

    image = bytearray()
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            dist, side, colour, wall_u = zbuffer[x]
            line = SIZE * 2.6 / max(dist, 0.05)
            top = SIZE / 2 - line / 2
            bottom = SIZE / 2 + line / 2
            if top <= y < bottom:
                light = 2.6 / (2.6 + dist * 1.05) * (0.75 if side else 1.0)
                # Mortar lines every 1/8 of the wall height for a brick look.
                v = (y - top) / max(line, 1.0)
                brick_row = int(v * 8)
                if (v * 8) % 1 < 0.07 or (wall_u * 3 + (brick_row % 2) * 0.5) % 1 < 0.045:
                    light *= 0.7
                rgb = shade(colour, light)
            elif y >= bottom:
                row_dist = (SIZE / 2) / (y - SIZE / 2 + 0.5)
                rgb = shade(FLOOR, 2.6 / (2.6 + row_dist * 1.05))
            else:
                row_dist = (SIZE / 2) / (SIZE / 2 - y + 0.5)
                rgb = shade(CEILING, 2.6 / (2.6 + row_dist * 1.05))
            row += bytes(rgb) + b"\xff"
        image += b"\x00" + bytes(row)
    return bytes(image)


def crosshair(raw: bytes) -> bytes:
    data = bytearray(raw)
    stride = 1 + SIZE * 4
    cx = cy = SIZE // 2
    # Broad ivory arms with a dark outline remain readable at hall thumbnail size.
    for extent, gap, thickness, colour in (
        (49, 12, 6, b"\x16\x12\x0e\xff"),
        (46, 15, 3, b"\xff\xf3\xd5\xff"),
    ):
        for d in range(-extent, extent + 1):
            if -gap < d < gap:
                continue
            for t in range(-thickness, thickness + 1):
                for x, y in ((cx + d, cy + t), (cx + t, cy + d)):
                    offset = y * stride + 1 + x * 4
                    data[offset : offset + 4] = colour
    return bytes(data)


def write_png(path: Path, raw: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "launch.png")
    args = parser.parse_args()
    write_png(args.output, crosshair(render()))
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
