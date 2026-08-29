#!/usr/bin/env python3
"""Generate a temporally resampled transparent Demo explosion atlas."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image


SOURCE_CANVAS_SIZE = (192, 192)
SOURCE_FRAMES = (
    ((158, 582, 40, 44), (76, 76)),
    ((202, 458, 56, 60), (68, 68)),
    ((86, 582, 68, 76), (60, 60)),
    ((2, 582, 80, 96), (56, 56)),
    ((106, 458, 92, 112), (52, 52)),
    ((2, 458, 100, 120), (48, 52)),
    ((138, 306, 108, 132), (44, 48)),
    ((138, 154, 116, 144), (40, 44)),
    ((138, 2, 124, 148), (36, 44)),
    ((2, 2, 132, 148), (32, 44)),
    ((2, 154, 132, 148), (32, 44)),
    ((2, 306, 132, 148), (32, 44)),
)


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=repository / "guest/apps/demo/assets/source/demo_explosion_12.png",
        help="original 12-frame RGBA explosion atlas",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "guest/apps/demo/assets/demo_sprite_sheet.png",
        help="first generated PNG atlas; additional sheets receive _1, _2, ... suffixes",
    )
    parser.add_argument(
        "--metadata",
        type=Path,
        default=repository / "build/atlas-review/explosion-30.json",
        help="generated atlas metadata for assets/manifest.json",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repository / "guest/apps/demo/assets/manifest.json",
        help="Demo asset manifest updated by --update-manifest",
    )
    parser.add_argument(
        "--update-manifest",
        action="store_true",
        help="replace the Demo sprite canvas and frame list in the asset manifest",
    )
    parser.add_argument(
        "--preview",
        type=Path,
        default=repository / "build/atlas-review/explosion-30-preview.webp",
        help="animated preview path, or an empty string to disable",
    )
    parser.add_argument("--frames", type=int, default=30, help="number of output frames")
    parser.add_argument("--fps", type=int, default=60, help="preview playback rate")
    parser.add_argument(
        "--canvas-size", type=int, default=256, help="logical alignment canvas for 2x sprite frames"
    )
    parser.add_argument("--sheets", type=int, default=3, help="number of equal-length atlas sheets")
    parser.add_argument(
        "--max-widths",
        default="456,496,648",
        help="comma-separated maximum widths, one per atlas sheet",
    )
    parser.add_argument("--max-dimension", type=int, default=720, help="Host texture dimension limit")
    return parser.parse_args()


def load_source_frames(source_path: Path) -> list[Image.Image]:
    with Image.open(source_path) as source_image:
        source = source_image.convert("RGBA")
    frames: list[Image.Image] = []
    for region, canvas_position in SOURCE_FRAMES:
        x, y, width, height = region
        canvas_x, canvas_y = canvas_position
        if x + width > source.width or y + height > source.height:
            raise ValueError(f"source frame {region} exceeds {source.width}x{source.height} image")
        canvas = Image.new("RGBA", SOURCE_CANVAS_SIZE, (0, 0, 0, 0))
        canvas.alpha_composite(source.crop((x, y, x + width, y + height)), canvas_position)
        frames.append(canvas)
    return frames


def premultiplied_blend(first: Image.Image, second: Image.Image, amount: float) -> Image.Image:
    """Blend straight-alpha RGBA images without producing dark transparent fringes."""
    inverse = 1.0 - amount
    pixels: list[tuple[int, int, int, int]] = []
    first_bytes = first.tobytes()
    second_bytes = second.tobytes()
    for offset in range(0, len(first_bytes), 4):
        left = first_bytes[offset : offset + 4]
        right = second_bytes[offset : offset + 4]
        left_alpha = left[3] / 255.0
        right_alpha = right[3] / 255.0
        alpha = inverse * left_alpha + amount * right_alpha
        if alpha <= 0.0:
            pixels.append((0, 0, 0, 0))
            continue
        channels = tuple(
            round((inverse * left[channel] * left_alpha + amount * right[channel] * right_alpha) / alpha)
            for channel in range(3)
        )
        pixels.append((*channels, round(alpha * 255.0)))
    output = Image.new("RGBA", first.size)
    output.putdata(pixels)
    return output


def resample_frames(source_frames: list[Image.Image], output_count: int) -> list[Image.Image]:
    if output_count < 2:
        raise ValueError("--frames must be at least 2")
    last_source_index = len(source_frames) - 1
    frames: list[Image.Image] = []
    for output_index in range(output_count):
        position = output_index * last_source_index / (output_count - 1)
        first_index = int(position)
        second_index = min(first_index + 1, last_source_index)
        amount = position - first_index
        frames.append(premultiplied_blend(source_frames[first_index], source_frames[second_index], amount))
    return frames


def resize_frames(frames: list[Image.Image], canvas_size: int) -> list[Image.Image]:
    if canvas_size <= 0 or canvas_size > 512:
        raise ValueError("--canvas-size must be between 1 and 512")
    if canvas_size == SOURCE_CANVAS_SIZE[0]:
        return frames
    return [
        frame.resize((canvas_size, canvas_size), resample=Image.Resampling.LANCZOS)
        for frame in frames
    ]


def trim_frame(frame: Image.Image, padding: int = 2) -> tuple[Image.Image, tuple[int, int]]:
    bounds = frame.getchannel("A").getbbox()
    if bounds is None:
        return Image.new("RGBA", (1, 1), (0, 0, 0, 0)), (frame.width // 2, frame.height // 2)
    left = max(0, bounds[0] - padding)
    top = max(0, bounds[1] - padding)
    right = min(frame.width, bounds[2] + padding)
    bottom = min(frame.height, bounds[3] + padding)
    return frame.crop((left, top, right, bottom)), (left, top)


def pack_frames(
    frames: list[Image.Image], max_width: int, spacing: int = 2
) -> tuple[Image.Image, list[dict[str, list[int]]]]:
    trimmed = [trim_frame(frame) for frame in frames]
    widest = max(frame.width for frame, _ in trimmed)
    if max_width < widest:
        raise ValueError(f"--max-width must be at least {widest}")

    placements: list[tuple[int, int]] = []
    cursor_x = 0
    cursor_y = 0
    row_height = 0
    atlas_width = 0
    for frame, _ in trimmed:
        if cursor_x > 0 and cursor_x + frame.width > max_width:
            cursor_x = 0
            cursor_y += row_height + spacing
            row_height = 0
        placements.append((cursor_x, cursor_y))
        atlas_width = max(atlas_width, cursor_x + frame.width)
        cursor_x += frame.width + spacing
        row_height = max(row_height, frame.height)
    atlas_height = cursor_y + row_height

    atlas = Image.new("RGBA", (atlas_width, atlas_height), (0, 0, 0, 0))
    metadata: list[dict[str, list[int]]] = []
    for (frame, canvas_position), (x, y) in zip(trimmed, placements, strict=True):
        atlas.alpha_composite(frame, (x, y))
        metadata.append(
            {
                "region": [x, y, frame.width, frame.height],
                "canvas_position": [canvas_position[0], canvas_position[1]],
            }
        )
    return atlas, metadata


def sheet_output_path(first_output: Path, sheet_index: int) -> Path:
    if sheet_index == 0:
        return first_output
    return first_output.with_name(f"{first_output.stem}_{sheet_index}{first_output.suffix}")


def update_manifest(
    manifest_path: Path,
    canvas_size: int,
    sheets: list[tuple[Path, list[dict[str, list[int]]]]],
) -> None:
    parsed = json.loads(manifest_path.read_text(encoding="utf-8"))
    assets = parsed.get("assets")
    if not isinstance(assets, list):
        raise ValueError("manifest assets must be a list")
    sprite_indices = [
        index
        for index, asset in enumerate(assets)
        if isinstance(asset, dict) and str(asset.get("name", "")).startswith("sprite.demo")
    ]
    if not sprite_indices:
        raise ValueError("manifest must contain a sprite.demo atlas")
    insert_at = sprite_indices[0]
    retained_assets = [asset for index, asset in enumerate(assets) if index not in sprite_indices]
    generated_assets = [
        {
            "name": f"sprite.demo{sheet_index}",
            "format": "png",
            "path": output_path.name,
            "atlas": {
                "index": sheet_index,
                "canvas": [canvas_size, canvas_size],
                "frames": frames_metadata,
            },
        }
        for sheet_index, (output_path, frames_metadata) in enumerate(sheets)
    ]
    retained_assets[insert_at:insert_at] = generated_assets
    parsed["assets"] = retained_assets
    manifest_path.write_text(json.dumps(parsed, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    if args.fps <= 0:
        raise ValueError("--fps must be positive")
    if args.sheets <= 0 or args.frames % args.sheets != 0:
        raise ValueError("--frames must divide evenly across a positive --sheets count")
    max_widths = [int(value) for value in args.max_widths.split(",")]
    if len(max_widths) != args.sheets or any(width <= 0 for width in max_widths):
        raise ValueError("--max-widths must provide one positive width per sheet")
    source_frames = load_source_frames(args.source)
    output_frames = resize_frames(resample_frames(source_frames, args.frames), args.canvas_size)
    frames_per_sheet = args.frames // args.sheets
    sheets: list[tuple[Path, list[dict[str, list[int]]]]] = []
    sheet_descriptions: list[str] = []
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for sheet_index, max_width in enumerate(max_widths):
        first_frame = sheet_index * frames_per_sheet
        atlas, frames_metadata = pack_frames(
            output_frames[first_frame : first_frame + frames_per_sheet], max_width
        )
        if atlas.width > args.max_dimension or atlas.height > args.max_dimension:
            raise ValueError(
                f"sheet {sheet_index} is {atlas.width}x{atlas.height}, exceeding "
                f"--max-dimension {args.max_dimension}"
            )
        output_path = sheet_output_path(args.output, sheet_index)
        atlas.save(output_path, format="PNG", optimize=True)
        sheets.append((output_path, frames_metadata))
        sheet_descriptions.append(f"{output_path.name}={atlas.width}x{atlas.height}")
    args.metadata.parent.mkdir(parents=True, exist_ok=True)
    args.metadata.write_text(
        json.dumps(
            {
                "canvas": [args.canvas_size, args.canvas_size],
                "frames_per_sheet": frames_per_sheet,
                "sheets": [
                    {"path": path.name, "frames": frames_metadata}
                    for path, frames_metadata in sheets
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    if args.update_manifest:
        update_manifest(args.manifest, args.canvas_size, sheets)
    if str(args.preview):
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        output_frames[0].save(
            args.preview,
            format="WEBP",
            save_all=True,
            append_images=output_frames[1:],
            duration=round(1000 / args.fps),
            loop=0,
            lossless=True,
        )
    print(f"Generated {len(output_frames)} frames: {', '.join(sheet_descriptions)}")


if __name__ == "__main__":
    main()
