#!/usr/bin/env python3
"""Generate Juicy Tilt's deterministic 480/720 runtime art and level header."""

from __future__ import annotations

import argparse
import json
import math
from collections import deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps


SOURCE_DIR = Path(__file__).resolve().parent
ASSETS_DIR = SOURCE_DIR.parent
APP_DIR = ASSETS_DIR.parent
LEVELS_PATH = SOURCE_DIR / "levels.json"

BG = (5, 8, 13, 255)
FLOOR = (7, 17, 26, 255)
FLOOR_DOT = (10, 27, 39, 255)
CYAN = (45, 226, 230, 255)
BLUE = (42, 140, 255, 255)
AMBER = (255, 176, 32, 255)
WALL_BLOCK_BORDER = (14, 31, 43, 255)
WALL_BLOCK_FILL = (38, 76, 97, 255)
WALL_BLOCK_MOTIF = (50, 96, 116, 255)
WALL_BLOCK_CORE = (29, 61, 80, 255)
ORANGE = (255, 119, 28, 255)
ROUTE_GRID = 6
BALL_RADIUS = 18.0
VISUAL_WALL_BLOCK_LENGTH = 20


def normalized_fan(values: list[int]) -> list[int]:
    if len(values) == 5:
        return [*values, 0, 0, 0]
    if len(values) == 8:
        return values
    raise ValueError("fan entries must contain 5 values (always on) or 8 values (pulsed)")


def route_position_blocked(level: dict[str, object], x: float, y: float) -> bool:
    pit_x, pit_y, pit_radius = level["pit"]
    pit_trigger_radius = pit_radius - BALL_RADIUS * 0.35 + float(level.get("pit_route_margin", 0))
    if x < BALL_RADIUS or x > 600.0 - BALL_RADIUS or y < BALL_RADIUS or y > 600.0 - BALL_RADIUS:
        return True
    for wall_x, wall_y, wall_width, wall_height in level["walls"]:
        if (wall_x - BALL_RADIUS <= x <= wall_x + wall_width + BALL_RADIUS and
                wall_y - BALL_RADIUS <= y <= wall_y + wall_height + BALL_RADIUS):
            return True
    return pit_radius > 0 and (x - pit_x) ** 2 + (y - pit_y) ** 2 <= pit_trigger_radius ** 2


def shortest_all_star_route(level: dict[str, object]) -> list[tuple[int, int]]:
    """Find the deterministic static shortest route used to audit hand-authored levels."""
    start_x, start_y = (round(value / ROUTE_GRID) for value in level["start"])

    def star_bits(x: int, y: int) -> int:
        return sum(1 << index for index, (star_x, star_y) in enumerate(level["stars"])
                   if (x * ROUTE_GRID - star_x) ** 2 + (y * ROUTE_GRID - star_y) ** 2 <= 36 ** 2)

    start = (start_x, start_y, star_bits(start_x, start_y))
    pending = deque([start])
    previous = {start: None}
    final = None
    while pending:
        state = pending.popleft()
        x, y, stars = state
        goal_x, goal_y, goal_radius = level["goal"]
        if stars == 0b111 and ((x * ROUTE_GRID - goal_x) ** 2 +
                              (y * ROUTE_GRID - goal_y) ** 2 <= goal_radius ** 2):
            final = state
            break
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            next_x, next_y = x + dx, y + dy
            candidate = (next_x, next_y, stars | star_bits(next_x, next_y))
            if (candidate in previous or
                    route_position_blocked(level, next_x * ROUTE_GRID, next_y * ROUTE_GRID)):
                continue
            previous[candidate] = state
            pending.append(candidate)
    if final is None:
        return []
    route = []
    while final is not None:
        route.append((final[0] * ROUTE_GRID, final[1] * ROUTE_GRID))
        final = previous[final]
    return list(reversed(route))


def shortest_goal_route_steps(level: dict[str, object]) -> int:
    start = tuple(round(value / ROUTE_GRID) for value in level["start"])
    pending = deque([(start, 0)])
    visited = {start}
    goal_x, goal_y, goal_radius = level["goal"]
    while pending:
        (x, y), steps = pending.popleft()
        if ((x * ROUTE_GRID - goal_x) ** 2 +
                (y * ROUTE_GRID - goal_y) ** 2 <= goal_radius ** 2):
            return steps
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            candidate = (x + dx, y + dy)
            if (candidate in visited or
                    route_position_blocked(level, candidate[0] * ROUTE_GRID, candidate[1] * ROUTE_GRID)):
                continue
            visited.add(candidate)
            pending.append((candidate, steps + 1))
    return 0


def route_hits_rect(route: list[tuple[int, int]], rect: list[int], padding: float = BALL_RADIUS) -> bool:
    left = rect[0] - padding
    top = rect[1] - padding
    right = rect[0] + rect[2] + padding
    bottom = rect[1] + rect[3] + padding
    for first, second in zip(route, route[1:]):
        minimum_progress = 0.0
        maximum_progress = 1.0
        for start, end, minimum, maximum in ((first[0], second[0], left, right),
                                              (first[1], second[1], top, bottom)):
            delta = end - start
            if delta == 0:
                if start < minimum or start > maximum:
                    minimum_progress = 1.0
                    maximum_progress = 0.0
                    break
                continue
            enter = (minimum - start) / delta
            leave = (maximum - start) / delta
            if enter > leave:
                enter, leave = leave, enter
            minimum_progress = max(minimum_progress, enter)
            maximum_progress = min(maximum_progress, leave)
        if minimum_progress <= maximum_progress:
            return True
    return any(left <= x <= right and top <= y <= bottom for x, y in route)


def route_hits_circle(route: list[tuple[int, int]], center_x: float, center_y: float, radius: float) -> bool:
    for first, second in zip(route, route[1:]):
        dx = second[0] - first[0]
        dy = second[1] - first[1]
        length_squared = dx * dx + dy * dy
        if length_squared == 0:
            closest_x, closest_y = first
        else:
            progress = max(0.0, min(1.0, ((center_x - first[0]) * dx + (center_y - first[1]) * dy) /
                                    length_squared))
            closest_x = first[0] + dx * progress
            closest_y = first[1] + dy * progress
        if (closest_x - center_x) ** 2 + (closest_y - center_y) ** 2 <= radius ** 2:
            return True
    return any((x - center_x) ** 2 + (y - center_y) ** 2 <= radius ** 2 for x, y in route)


def validate_level_challenges(level: dict[str, object], level_number: int) -> None:
    static_route = shortest_all_star_route(level)
    route = [tuple(point) for point in level.get("solution", [])] or static_route
    if not route:
        raise ValueError(f"level {level_number}: no all-star route for challenge audit")
    minimum_detour = int(level.get("minimum_star_detour_per_mille", 0))
    if minimum_detour:
        direct_steps = shortest_goal_route_steps(level)
        if direct_steps == 0 or (len(static_route) - 1) * 1000 < direct_steps * minimum_detour:
            raise ValueError(f"level {level_number}: stars do not require enough route detour")
    for index, raw_fan in enumerate(level.get("fans", []), start=1):
        fan_x, fan_y, radius, *_ = normalized_fan(raw_fan)
        if not route_hits_circle(route, fan_x, fan_y, radius):
            raise ValueError(f"level {level_number}: fan {index} misses the all-star route")
    ice = level["ice"]
    if ice[2] > 0 and ice[3] > 0 and not route_hits_rect(route, ice):
        raise ValueError(f"level {level_number}: ice misses the all-star route")
    bumper_x, bumper_y, bumper_radius = level["bumper"]
    if bumper_radius > 0:
        end_x, end_y = level["bumper_end"]
        swept = [min(bumper_x, end_x) - bumper_radius, min(bumper_y, end_y) - bumper_radius,
                 abs(end_x - bumper_x) + bumper_radius * 2, abs(end_y - bumper_y) + bumper_radius * 2]
        if not route_hits_rect(route, swept):
            raise ValueError(f"level {level_number}: bumper misses the all-star route")
    moving_wall = level.get("moving_wall", [0] * 7)
    if moving_wall[6] and not route_hits_rect(route, moving_wall[:4]):
        raise ValueError(f"level {level_number}: moving wall misses the all-star route")
    gate = level.get("gate", [0] * 7)
    if gate[4] and not route_hits_rect(route, gate[:4]):
        raise ValueError(f"level {level_number}: timed gate misses the all-star route")
    pressure_gate = level.get("pressure_gate", [0] * 7)
    if pressure_gate[2]:
        plate_x, plate_y, plate_radius = pressure_gate[:3]
        if not route_hits_circle(route, plate_x, plate_y, plate_radius + BALL_RADIUS):
            raise ValueError(f"level {level_number}: pressure plate is optional")
        if not route_hits_rect(route, pressure_gate[3:7]):
            raise ValueError(f"level {level_number}: pressure door misses the all-star route")
    portals = level.get("portals", [0] * 6)
    if portals[2]:
        delta_x = abs(portals[0] - portals[3])
        delta_y = abs(portals[1] - portals[4])
        minimum_portal_distance = 240
        if delta_x + delta_y < minimum_portal_distance:
            raise ValueError(f"level {level_number}: portal endpoints are visually too close")
        entrance_x, entrance_y, entrance_radius = portals[:3]
        if route_hits_circle(route, entrance_x, entrance_y, entrance_radius + 3):
            raise ValueError(f"level {level_number}: intended route enters the portal trap")
        destination_x, destination_y, destination_radius = portals[3:6]
        if not route_hits_circle(route, destination_x, destination_y, destination_radius):
            raise ValueError(f"level {level_number}: portal return point is unrelated to the all-star route")


def validate_level_routes(level: dict[str, object], level_number: int) -> None:
    """Reject layouts whose walls and pit make a required target unreachable."""
    def blocked(x: float, y: float) -> bool:
        return route_position_blocked(level, x, y)

    start = tuple(round(value / ROUTE_GRID) for value in level["start"])
    if blocked(start[0] * ROUTE_GRID, start[1] * ROUTE_GRID):
        raise ValueError(f"level {level_number}: start is blocked")
    pending = deque([start])
    reachable = {start}
    while pending:
        x, y = pending.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            candidate = (x + dx, y + dy)
            if candidate in reachable or blocked(candidate[0] * ROUTE_GRID, candidate[1] * ROUTE_GRID):
                continue
            reachable.add(candidate)
            pending.append(candidate)

    targets = [("goal", level["goal"][:2], float(level["goal"][2]))]
    targets.extend((f"star {index + 1}", point, BALL_RADIUS + 18.0)
                   for index, point in enumerate(level["stars"]))
    for name, (target_x, target_y), trigger_radius in targets:
        can_reach = any((cell_x * ROUTE_GRID - target_x) ** 2 +
                        (cell_y * ROUTE_GRID - target_y) ** 2 <= trigger_radius ** 2
                        for cell_x, cell_y in reachable)
        if not can_reach:
            raise ValueError(f"level {level_number}: {name} is unreachable when the pit is active")


def fitted_material(filename: str, size: tuple[int, int]) -> Image.Image:
    """Crop an ImageGen material master without baking gameplay geometry into it."""
    material = Image.open(SOURCE_DIR / filename).convert("RGB")
    return ImageOps.fit(material, size, method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))


def draw_flat_wall_block(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], border: int) -> None:
    x0, y0, x1, y1 = box
    draw.rectangle(box, fill=WALL_BLOCK_BORDER)
    if x1 - x0 + 1 <= border * 2 or y1 - y0 + 1 <= border * 2:
        return
    draw.rectangle((x0 + border, y0 + border, x1 - border, y1 - border), fill=WALL_BLOCK_FILL)
    center_x = (x0 + x1) // 2
    center_y = (y0 + y1) // 2
    motif_radius = max(1, round(min(x1 - x0 + 1, y1 - y0 + 1) * 0.27))
    draw.polygon(((center_x, center_y - motif_radius), (center_x + motif_radius, center_y),
                  (center_x, center_y + motif_radius), (center_x - motif_radius, center_y)),
                 fill=WALL_BLOCK_MOTIF)
    core_radius = max(1, motif_radius // 3)
    draw.polygon(((center_x, center_y - core_radius), (center_x + core_radius, center_y),
                  (center_x, center_y + core_radius), (center_x - core_radius, center_y)),
                 fill=WALL_BLOCK_CORE)


def supersampled_frame(frame_pixels: int, painter) -> Image.Image:
    factor = 4
    size = frame_pixels * factor
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    painter(image, ImageDraw.Draw(image), size)
    return image.resize((frame_pixels, frame_pixels), Image.Resampling.LANCZOS)


def render_board_base(output_pixels: int) -> Image.Image:
    supersample = 3
    canvas = output_pixels * supersample
    scale = canvas / 600.0
    image = Image.new("RGBA", (canvas, canvas), BG)
    draw = ImageDraw.Draw(image)
    radius = round(24 * scale)
    draw.rounded_rectangle((0, 0, canvas - 1, canvas - 1), radius=radius, fill=(3, 9, 15, 255))
    rim = round(8 * scale)
    draw.rounded_rectangle((rim, rim, canvas - 1 - rim, canvas - 1 - rim),
                           radius=max(1, radius - rim), fill=FLOOR)
    inner_rim = round(14 * scale)
    draw.rounded_rectangle((inner_rim, inner_rim, canvas - 1 - inner_rim, canvas - 1 - inner_rim),
                           radius=max(1, radius - inner_rim), outline=(10, 31, 45, 255),
                           width=max(1, round(2 * scale)))
    dot_step = max(14, round(40 * scale))
    dot_radius = max(1, round(scale))
    for y in range(dot_step // 2, canvas, dot_step):
        for x in range(dot_step // 2, canvas, dot_step):
            draw.ellipse((x - dot_radius, y - dot_radius, x + dot_radius, y + dot_radius), fill=FLOOR_DOT)
    return image.resize((output_pixels, output_pixels), Image.Resampling.LANCZOS).convert("RGB")


def render_board_frame(output_pixels: int) -> Image.Image:
    supersample = 3
    canvas = output_pixels * supersample
    scale = canvas / 600.0
    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    radius = round(24 * scale)
    rail_inset = round(3 * scale)
    draw.rounded_rectangle((rail_inset, rail_inset, canvas - 1 - rail_inset, canvas - 1 - rail_inset),
                           radius=max(1, radius - rail_inset), outline=(24, 43, 59, 255),
                           width=max(3, round(14 * scale)))
    highlight_inset = round(7 * scale)
    draw.rounded_rectangle((highlight_inset, highlight_inset, canvas - 1 - highlight_inset,
                            canvas - 1 - highlight_inset), radius=max(1, radius - highlight_inset),
                           outline=(96, 130, 160, 255), width=max(1, round(2 * scale)))
    draw.rounded_rectangle((0, 0, canvas - 1, canvas - 1), radius=radius,
                           outline=(37, 205, 231, 230), width=max(1, round(2 * scale)))
    return image.resize((output_pixels, output_pixels), Image.Resampling.LANCZOS)


def paint_wall_block(image: Image.Image) -> None:
    """Paint one direction-neutral 2D block with no lighting, texture, bevel, or shadow."""
    size = image.width
    draw_flat_wall_block(ImageDraw.Draw(image), (0, 0, size - 1, size - 1), max(1, round(size * 4 / 96)))


def paint_ice_tile(image: Image.Image) -> None:
    size = image.width
    padding = max(2, round(size * 7 / 96))
    material = fitted_material("ice-master.png", (size - padding * 2, size - padding * 2))
    material = ImageEnhance.Color(material).enhance(0.82)
    material = ImageEnhance.Brightness(material).enhance(0.78)
    mask = Image.new("L", material.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, material.width - 1, material.height - 1),
                                           radius=max(3, round(size * 8 / 96)), fill=255)
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).rounded_rectangle((padding // 2, padding // 2, size - padding // 2,
                                            size - padding // 2), radius=max(3, round(size * 12 / 96)),
                                           fill=(19, 176, 255, 110))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(max(1, round(size * 5 / 96)))))
    image.paste(material, (padding, padding), mask)
    ImageDraw.Draw(image).rounded_rectangle((padding, padding, size - padding - 1, size - padding - 1),
                                             radius=max(3, round(size * 8 / 96)),
                                             outline=(116, 234, 255, 255),
                                             width=max(1, round(size * 2 / 96)))


def paint_airflow_tile(image: Image.Image, direction_index: int) -> None:
    size = image.width
    angle = -math.pi / 2 + direction_index * math.pi / 4
    direction_x = math.cos(angle)
    direction_y = math.sin(angle)
    normal_x = -direction_y
    normal_y = direction_x
    glow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)
    center = size / 2
    for lane in (-1, 0, 1):
        lateral = lane * size * 0.14
        start_distance = size * 0.22
        end_distance = size * (0.42 + 0.05 * (lane + 1))
        start = (center + direction_x * start_distance + normal_x * lateral,
                 center + direction_y * start_distance + normal_y * lateral)
        end = (center + direction_x * end_distance + normal_x * lateral,
               center + direction_y * end_distance + normal_y * lateral)
        draw.line((*start, *end), fill=(66, 224, 245, 130), width=max(2, round(size * 2 / 96)))
    image.alpha_composite(glow.filter(ImageFilter.GaussianBlur(max(1, round(size / 96)))))


def render_board_tiles(frame_pixels: int) -> Image.Image:
    painters = [paint_wall_block, paint_ice_tile]
    painters.extend(lambda image, index=index: paint_airflow_tile(image, index) for index in range(8))
    columns = 5
    rows = math.ceil(len(painters) / columns)
    atlas = Image.new("RGBA", (columns * frame_pixels, rows * frame_pixels), (0, 0, 0, 0))
    for index, painter in enumerate(painters):
        frame = Image.new("RGBA", (frame_pixels, frame_pixels), (0, 0, 0, 0))
        painter(frame)
        atlas.paste(frame, ((index % columns) * frame_pixels, (index // columns) * frame_pixels), frame)
    return atlas


def glow_layer(size: int, color: tuple[int, int, int, int], radius: float, blur: float) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    center = size / 2
    draw.ellipse((center - radius, center - radius, center + radius, center + radius), fill=color)
    return layer.filter(ImageFilter.GaussianBlur(blur))


def paint_marble(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
    center = size / 2
    radius = size * 0.28
    image.alpha_composite(glow_layer(size, (33, 210, 255, 120), radius * 1.05, size * 0.045))
    for ring in range(round(radius), 0, -1):
        t = ring / radius
        color = (round(40 + 35 * (1 - t)), round(190 + 55 * (1 - t)), 255, 255)
        draw.ellipse((center - ring, center - ring, center + ring, center + ring), fill=color)
    draw.ellipse((center - radius, center - radius, center + radius, center + radius),
                 outline=(210, 255, 255, 255), width=max(4, round(size * 0.035)))
    highlight = radius * 0.34
    draw.ellipse((center - radius * 0.48, center - radius * 0.54,
                  center - radius * 0.48 + highlight, center - radius * 0.54 + highlight),
                 fill=(255, 255, 255, 220))


def paint_goal(phase: int):
    def painter(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
        center = size / 2
        pulse = (phase + 1) / 4.0
        radius = size * (0.27 + 0.025 * pulse)
        image.alpha_composite(glow_layer(size, (35, 235, 225, 95), radius, size * (0.035 + 0.01 * pulse)))
        for fraction, alpha in ((1.0, 235), (0.72, 220), (0.42, 255)):
            r = radius * fraction
            draw.ellipse((center - r, center - r, center + r, center + r), outline=(45, 238, 232, alpha),
                         width=max(4, round(size * 0.035)))
        core = radius * 0.2
        draw.ellipse((center - core, center - core, center + core, center + core), fill=(80, 255, 244, 230))
    return painter


def paint_star(phase: int):
    def painter(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
        pulse = (0.93, 1.0, 1.06, 1.0)[phase]
        side = round(size * 0.62 * pulse)
        star = fit_master("hud-star-filled-master.png", side, side, max(1, round(side * 0.035)))
        image.alpha_composite(star, ((size - side) // 2, (size - side) // 2))
    return painter


def paint_pit(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
    center = size / 2
    radius = size * 0.31
    image.alpha_composite(glow_layer(size, (255, 135, 20, 75), radius, size * 0.035))
    for ring in range(round(radius), 0, -1):
        shade = round(3 + 17 * (ring / radius))
        draw.ellipse((center - ring, center - ring, center + ring, center + ring), fill=(shade, shade + 2, shade + 4, 255))
    draw.ellipse((center - radius, center - radius, center + radius, center + radius),
                 outline=(255, 156, 25, 245), width=max(4, round(size * 0.04)))


def paint_bumper(phase: int):
    def painter(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
        center = size / 2
        pulse = (1.0, 1.08, 0.96)[phase]
        radius = size * 0.25 * pulse
        image.alpha_composite(glow_layer(size, (255, 105, 20, 100), radius, size * 0.04))
        draw.ellipse((center - radius, center - radius, center + radius, center + radius),
                     fill=(147, 48, 8, 255), outline=(255, 139, 32, 255), width=max(5, round(size * 0.045)))
        inner = radius * 0.48
        draw.ellipse((center - inner, center - inner, center + inner, center + inner),
                     fill=(255, 126, 24, 255), outline=(255, 205, 90, 255), width=max(3, round(size * 0.025)))
    return painter


def paint_spark(phase: int):
    def painter(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
        center = size / 2
        radius = size * (0.13 + phase * 0.07)
        alpha = 255 - phase * 65
        width = max(3, round(size * 0.026))
        for index in range(8):
            angle = index * math.pi / 4
            inner = radius * 0.45
            draw.line((center + math.cos(angle) * inner, center + math.sin(angle) * inner,
                       center + math.cos(angle) * radius, center + math.sin(angle) * radius),
                      fill=(120, 245, 255, alpha), width=width)
    return painter


def render_objects(frame_pixels: int) -> Image.Image:
    painters = [
        paint_marble,
        paint_goal(0), paint_goal(1), paint_goal(2), paint_goal(3),
        paint_star(0), paint_star(1), paint_star(2), paint_star(3),
        paint_pit,
        paint_bumper(0), paint_bumper(1), paint_bumper(2),
        paint_spark(0), paint_spark(1), paint_spark(2),
    ]
    sheet = Image.new("RGBA", (frame_pixels * 4, frame_pixels * 4), (0, 0, 0, 0))
    for index, painter in enumerate(painters):
        frame = supersampled_frame(frame_pixels, painter)
        sheet.alpha_composite(frame, ((index % 4) * frame_pixels, (index // 4) * frame_pixels))
    return sheet


def render_fans(frame_pixels: int) -> Image.Image:
    """Build an eight-frame fan strip with a pixel-stable housing."""
    factor = 4
    size = frame_pixels * factor
    base = fit_master("fan-master.png", size, size, max(2, round(size * 0.035)))
    center = size / 2.0
    rotor_radius = size * 0.305
    rotor_mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(rotor_mask).ellipse((center - rotor_radius, center - rotor_radius,
                                       center + rotor_radius, center + rotor_radius), fill=255)
    rotor_mask = rotor_mask.filter(ImageFilter.GaussianBlur(max(1, round(size * 0.004))))
    sheet = Image.new("RGBA", (frame_pixels * 8, frame_pixels), (0, 0, 0, 0))
    for phase in range(8):
        # Four blades repeat every 90 degrees; 8 steps therefore loop at 11.25 degrees.
        rotated_rotor = base.rotate(phase * 11.25, resample=Image.Resampling.BICUBIC, expand=False)
        frame = base.copy()
        frame.paste(rotated_rotor, (0, 0), rotor_mask)
        frame = frame.resize((frame_pixels, frame_pixels), Image.Resampling.LANCZOS)
        sheet.alpha_composite(frame, (phase * frame_pixels, 0))
    return sheet


def fit_master(filename: str, width: int, height: int, padding: int) -> Image.Image:
    return fit_rgba(Image.open(SOURCE_DIR / filename).convert("RGBA"), width, height, padding, filename)


def fit_rgba(source: Image.Image, width: int, height: int, padding: int, label: str) -> Image.Image:
    alpha = source.getchannel("A")
    visible = alpha.point(lambda value: 255 if value >= 3 else 0)
    bounds = visible.getbbox()
    if bounds is None:
        raise ValueError(f"{label} has no visible alpha")
    source = source.crop(bounds)
    available_width = width - padding * 2
    available_height = height - padding * 2
    scale = min(available_width / source.width, available_height / source.height)
    resized = source.resize((max(1, round(source.width * scale)), max(1, round(source.height * scale))),
                            Image.Resampling.LANCZOS)
    output = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    output.alpha_composite(resized, ((width - resized.width) // 2, (height - resized.height) // 2))
    return output


def chroma_key_master(filename: str) -> Image.Image:
    source = Image.open(SOURCE_DIR / filename).convert("RGB")
    keyed = Image.new("RGBA", source.size, (0, 0, 0, 0))
    output_pixels = []
    for red, green, blue in source.get_flattened_data():
        distance = math.sqrt(red * red + (green - 255) * (green - 255) + blue * blue)
        alpha = max(0, min(255, round((distance - 24.0) * 3.2)))
        clean_green = min(green, max(red, blue) + 18)
        output_pixels.append((red, clean_green, blue, alpha))
    keyed.putdata(output_pixels)
    return keyed


def shift_hue(source: Image.Image, amount: int) -> Image.Image:
    alpha = source.getchannel("A")
    hue, saturation, value = source.convert("RGB").convert("HSV").split()
    hue = hue.point(lambda channel: (channel + amount) % 256)
    shifted = Image.merge("HSV", (hue, saturation, value)).convert("RGBA")
    shifted.putalpha(alpha)
    return shifted


def render_mechanics(frame_pixels: int) -> Image.Image:
    sheet = Image.new("RGBA", (frame_pixels * 4, frame_pixels * 2), (0, 0, 0, 0))
    padding = max(2, frame_pixels // 18)
    blocker_h = fit_master("blocker-master.png", frame_pixels, frame_pixels, padding)
    blocker_v = blocker_h.rotate(90, resample=Image.Resampling.BICUBIC, expand=False)
    gate_h = fit_master("state-door-master.png", frame_pixels, frame_pixels, padding)
    gate_v = gate_h.rotate(90, resample=Image.Resampling.BICUBIC, expand=False)
    pressure_on = fit_master("pressure-plate-master.png", frame_pixels, frame_pixels, padding)
    pressure_off = ImageEnhance.Color(pressure_on).enhance(0.42)
    pressure_off = ImageEnhance.Brightness(pressure_off).enhance(0.54)
    portal_a = fit_master("portal-master.png", frame_pixels, frame_pixels, padding)
    portal_b = shift_hue(portal_a, 48)
    frames = (blocker_h, blocker_v, gate_h, gate_v,
              pressure_off, pressure_on, portal_a, portal_b)
    for index, frame in enumerate(frames):
        sheet.alpha_composite(frame, ((index % 4) * frame_pixels, (index // 4) * frame_pixels))
    return sheet


def render_title(width: int, height: int) -> Image.Image:
    return fit_master("title-master.png", width, height, 1)


def paint_tilt_icon(image: Image.Image, draw: ImageDraw.ImageDraw, size: int) -> None:
    cyan = (45, 236, 237, 255)
    line_width = max(3, round(size * 0.045))
    draw.arc((size * 0.02, size * 0.18, size * 0.38, size * 0.82), 105, 255, fill=cyan, width=line_width)
    draw.arc((size * 0.62, size * 0.18, size * 0.98, size * 0.82), -75, 75, fill=cyan, width=line_width)
    phone = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    phone_draw = ImageDraw.Draw(phone)
    phone_draw.rounded_rectangle((size * 0.27, size * 0.20, size * 0.73, size * 0.80),
                                 radius=max(3, round(size * 0.07)), fill=(7, 15, 23, 255),
                                 outline=(242, 248, 250, 255), width=line_width)
    phone_draw.ellipse((size * 0.47, size * 0.70, size * 0.53, size * 0.76), fill=(242, 248, 250, 255))
    phone = phone.rotate(-9, resample=Image.Resampling.BICUBIC)
    image.alpha_composite(phone)


def render_hud_icons(frame_pixels: int) -> Image.Image:
    sheet = Image.new("RGBA", (frame_pixels * 3, frame_pixels), (0, 0, 0, 0))
    sheet.alpha_composite(fit_master("hud-star-filled-master.png", frame_pixels, frame_pixels, 1), (0, 0))
    sheet.alpha_composite(fit_master("hud-star-empty-master.png", frame_pixels, frame_pixels, 1), (frame_pixels, 0))
    sheet.alpha_composite(supersampled_frame(frame_pixels, paint_tilt_icon), (frame_pixels * 2, 0))
    return sheet


def emit_header(levels: list[dict[str, object]], output_path: Path | None = None) -> None:
    lines = [
        "// Generated by assets/source/generate_gameplay_assets.py. Do not edit by hand.",
        "#ifndef MICROPIXEL_TILT_LEVEL_DATA_HPP",
        "#define MICROPIXEL_TILT_LEVEL_DATA_HPP",
        "",
        '#include "apps/tilt/tilt_common.hpp"',
        "",
        "namespace tilt {",
        "",
        "struct LevelData final {",
        "    uint32_t required_stars{};",
        "    uint32_t par_time_ms{};",
        "    PointF start{};",
        "    CircleFeature goal{};",
        "    CircleFeature pit{};",
        "    CircleFeature bumper{};",
        "    PointF bumper_end{};",
        "    uint32_t bumper_period_ms{};",
        "    RectFeature ice{};",
        "    PointF stars[kStarCount]{};",
        "    FanFeature fans[kMaximumFanCount]{};",
        "    uint32_t fan_count{};",
        "    MovingWallFeature moving_wall{};",
        "    TimedGateFeature gate{};",
        "    PressureGateFeature pressure_gate{};",
        "    PortalPairFeature portals{};",
        "#if defined(MICROPIXEL_MODEL_TESTING)",
        "    const PointF* solution{};",
        "    uint32_t solution_count{};",
        "#endif",
        "    const WallRect* walls{};",
        "    uint32_t wall_count{};",
        "    const VisualWallFeature* visual_walls{};",
        "    uint32_t visual_wall_count{};",
        "    const WallJointFeature* wall_joints{};",
        "    uint32_t wall_joint_count{};",
        "};",
        "",
    ]
    for level_index, level in enumerate(levels, start=1):
        lines.append(f"inline constexpr WallRect kLevel{level_index}Walls[] = {{")
        for wall in level["walls"]:
            lines.append(f"    {{{wall[0]}, {wall[1]}, {wall[2]}, {wall[3]}}},")
        lines.extend(["};", ""])
        visual_walls, visual_joints = visual_wall_topology(level)
        lines.append(f"inline constexpr VisualWallFeature kLevel{level_index}VisualWalls[] = {{")
        for x, y, width, height, horizontal, start_connected, end_connected in visual_walls:
            lines.append(f"    {{{{{x}, {y}, {width}, {height}}}, {horizontal}U, "
                         f"{start_connected}U, {end_connected}U}},")
        lines.extend(["};", ""])
        lines.append(f"inline constexpr WallJointFeature kLevel{level_index}WallJoints[] = {{")
        for x, y, directions in visual_joints:
            lines.append(f"    {{{x}, {y}, {directions}U}},")
        lines.extend(["};", ""])
    lines.append("#if defined(MICROPIXEL_MODEL_TESTING)")
    for level_index, level in enumerate(levels, start=1):
        solution = level.get("solution", [])
        if solution:
            lines.append(f"inline constexpr PointF kLevel{level_index}Solution[] = {{")
            for x, y in solution:
                lines.append(f"    {{{x}.0F, {y}.0F}},")
            lines.extend(["};", ""])
    lines.extend(["#endif", ""])
    lines.append("inline constexpr LevelData kLevels[] = {")
    for level_index, level in enumerate(levels, start=1):
        stars = ", ".join(f"{{{x}.0F, {y}.0F}}" for x, y in level["stars"])
        fans = ", ".join(f"{{{x}, {y}, {radius}, {force_x}, {force_y}, {period}, {active}, {phase}}}"
                          for x, y, radius, force_x, force_y, period, active, phase
                          in (normalized_fan(fan) for fan in level.get("fans", [])))
        fans = fans if fans else "{}"
        moving_wall = level.get("moving_wall", [0, 0, 0, 0, 0, 0, 0])
        gate = level.get("gate", [0, 0, 0, 0, 0, 0, 0])
        pressure_gate = level.get("pressure_gate", [0, 0, 0, 0, 0, 0, 0])
        portals = level.get("portals", [0, 0, 0, 0, 0, 0])
        solution = level.get("solution", [])
        lines.extend([
            "    {",
            f"        .required_stars = {level.get('required_stars', 3)}U,",
            f"        .par_time_ms = {level.get('par_time_ms', 0)}U,",
            f"        .start = {{{level['start'][0]}.0F, {level['start'][1]}.0F}},",
            f"        .goal = {{{', '.join(map(str, level['goal']))}}},",
            f"        .pit = {{{', '.join(map(str, level['pit']))}}},",
            f"        .bumper = {{{', '.join(map(str, level['bumper']))}}},",
            f"        .bumper_end = {{{level['bumper_end'][0]}.0F, {level['bumper_end'][1]}.0F}},",
            f"        .bumper_period_ms = {level['bumper_period_ms']}U,",
            f"        .ice = {{{', '.join(map(str, level['ice']))}}},",
            f"        .stars = {{{stars}}},",
            f"        .fans = {{{fans}}},",
            f"        .fan_count = {len(level.get('fans', []))}U,",
            f"        .moving_wall = {{{{{moving_wall[0]}, {moving_wall[1]}, {moving_wall[2]}, {moving_wall[3]}}}, "
            f"{{{moving_wall[4]}.0F, {moving_wall[5]}.0F}}, {moving_wall[6]}U}},",
            f"        .gate = {{{{{gate[0]}, {gate[1]}, {gate[2]}, {gate[3]}}}, {gate[4]}U, {gate[5]}U, "
            f"{gate[6]}U}},",
            f"        .pressure_gate = {{{{{pressure_gate[0]}, {pressure_gate[1]}, {pressure_gate[2]}}}, "
            f"{{{pressure_gate[3]}, {pressure_gate[4]}, {pressure_gate[5]}, {pressure_gate[6]}}}}},",
            f"        .portals = {{{{{portals[0]}, {portals[1]}, {portals[2]}}}, "
            f"{{{portals[3]}, {portals[4]}, {portals[5]}}}}},",
            "#if defined(MICROPIXEL_MODEL_TESTING)",
            f"        .solution = {'kLevel' + str(level_index) + 'Solution' if solution else 'nullptr'},",
            f"        .solution_count = {len(solution)}U,",
            "#endif",
            f"        .walls = kLevel{level_index}Walls,",
            f"        .wall_count = sizeof(kLevel{level_index}Walls) / sizeof(kLevel{level_index}Walls[0]),",
            f"        .visual_walls = kLevel{level_index}VisualWalls,",
            f"        .visual_wall_count = sizeof(kLevel{level_index}VisualWalls) / "
            f"sizeof(kLevel{level_index}VisualWalls[0]),",
            f"        .wall_joints = kLevel{level_index}WallJoints,",
            f"        .wall_joint_count = sizeof(kLevel{level_index}WallJoints) / "
            f"sizeof(kLevel{level_index}WallJoints[0]),",
            "    },",
        ])
    lines.extend([
        "};",
        "",
        "inline constexpr uint32_t kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);",
        "",
        "}  // namespace tilt",
        "",
        "#endif",
        "",
    ])
    (output_path or APP_DIR / "tilt_level_data.hpp").write_text("\n".join(lines), encoding="utf-8")


def visual_wall_topology(level: dict[str, object]) -> tuple[list[tuple[int, int, int, int, int, int, int]],
                                                                  list[tuple[int, int, int]]]:
    walls = level.get("walls", [])
    generated_grid = bool(level.get("generator"))
    generator = level.get("generator", {})
    grid_size = int(generator.get("grid_size", 6))
    cell_size = int(generator.get("cell_size", 90))
    origin = int(generator.get("origin", 30))
    grid_nodes = {origin + index * cell_size for index in range(1, grid_size)}

    def horizontal(wall: list[int]) -> bool:
        if generated_grid:
            center_x = wall[0] + wall[2] // 2
            center_y = wall[1] + wall[3] // 2
            if wall[2] == 18 and center_x in grid_nodes:
                return False
            if wall[3] == 18 and center_y in grid_nodes:
                return True
        return wall[2] >= wall[3]

    grouped: dict[tuple[bool, int], list[tuple[int, int]]] = {}
    for wall in walls:
        is_horizontal = horizontal(wall)
        axis = wall[1] + wall[3] // 2 if is_horizontal else wall[0] + wall[2] // 2
        interval = ((wall[0], wall[0] + wall[2]) if is_horizontal else
                    (wall[1], wall[1] + wall[3]))
        grouped.setdefault((is_horizontal, axis), []).append(interval)

    merged: dict[tuple[bool, int], list[tuple[int, int]]] = {}
    for key, intervals in grouped.items():
        merged_intervals: list[tuple[int, int]] = []
        for start, end in sorted(intervals):
            if merged_intervals and start <= merged_intervals[-1][1]:
                merged_intervals[-1] = (merged_intervals[-1][0], max(merged_intervals[-1][1], end))
            else:
                merged_intervals.append((start, end))
        merged[key] = merged_intervals

    horizontal_lines = [(axis, start, end) for (is_horizontal, axis), intervals in merged.items()
                        if is_horizontal for start, end in intervals]
    vertical_lines = [(axis, start, end) for (is_horizontal, axis), intervals in merged.items()
                      if not is_horizontal for start, end in intervals]
    node_masks: dict[tuple[int, int], int] = {}
    for y, left, right in horizontal_lines:
        for x, top, bottom in vertical_lines:
            if not (left <= x <= right and top <= y <= bottom):
                continue
            directions = ((1 if top < y else 0) | (2 if right > x else 0) |
                          (4 if bottom > y else 0) | (8 if left < x else 0))
            if directions in (3, 6, 12, 9, 7, 14, 13, 11, 15):
                node_masks[(x, y)] = directions

    half_thickness = 9
    visual_segments: list[tuple[int, int, int, int, int, int, int]] = []

    def split_line(horizontal_line: bool, axis: int, start: int, end: int) -> None:
        nodes = sorted((x if horizontal_line else y) for x, y in node_masks
                       if (y == axis if horizontal_line else x == axis) and start <= (x if horizontal_line else y) <= end)
        cursor = start
        start_connected = False
        for node in nodes:
            piece_end = node - half_thickness
            if piece_end > cursor:
                rect = ((cursor, axis - half_thickness, piece_end - cursor, 18) if horizontal_line else
                        (axis - half_thickness, cursor, 18, piece_end - cursor))
                visual_segments.append((*rect, int(horizontal_line), int(start_connected), 1))
            cursor = node + half_thickness
            start_connected = True
        if end > cursor:
            rect = ((cursor, axis - half_thickness, end - cursor, 18) if horizontal_line else
                    (axis - half_thickness, cursor, 18, end - cursor))
            visual_segments.append((*rect, int(horizontal_line), int(start_connected), 0))

    for axis, start, end in horizontal_lines:
        split_line(True, axis, start, end)
    for axis, start, end in vertical_lines:
        split_line(False, axis, start, end)

    visual_segments = [segment for segment in visual_segments
                       if (segment[2] if segment[4] else segment[3]) >= 9]
    visible_rectangles = [(segment[0], segment[1], segment[0] + segment[2], segment[1] + segment[3])
                          for segment in visual_segments]
    visible_rectangles.extend((x - half_thickness, y - half_thickness,
                               x + half_thickness, y + half_thickness)
                              for x, y in node_masks)
    for wall in walls:
        left, top, width, height = wall
        right = left + width
        bottom = top + height
        if any(min(right, visible_right) > max(left, visible_left) and
               min(bottom, visible_bottom) > max(top, visible_top)
               for visible_left, visible_top, visible_right, visible_bottom in visible_rectangles):
            continue
        is_horizontal = width >= height
        if is_horizontal:
            rect = (left, top + height // 2 - half_thickness, width, half_thickness * 2)
        else:
            rect = (left + width // 2 - half_thickness, top, half_thickness * 2, height)
        visual_segments.append((*rect, int(is_horizontal), 0, 0))
        visible_rectangles.append((rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]))

    visual_segments.sort(key=lambda segment: (segment[1], segment[0], segment[4]))
    visual_joints = sorted(((x, y, directions) for (x, y), directions in node_masks.items()),
                           key=lambda joint: (joint[1], joint[0], joint[2]))
    return visual_segments, visual_joints


def visual_wall_joints(level: dict[str, object]) -> list[tuple[int, int, int]]:
    return visual_wall_topology(level)[1]


def visual_wall_blocks(level: dict[str, object]) -> list[tuple[int, int, int, int]]:
    visual_walls, visual_joints = visual_wall_topology(level)
    blocks: list[tuple[int, int, int, int]] = []
    for x, y, width, height, horizontal, _, _ in visual_walls:
        length = width if horizontal else height
        if length < 9:
            continue
        count = (length + VISUAL_WALL_BLOCK_LENGTH - 1) // VISUAL_WALL_BLOCK_LENGTH
        base_length = length // count
        longer_block_count = length % count
        offset = 0
        for index in range(count):
            block_length = base_length + (1 if index < longer_block_count else 0)
            blocks.append((x + offset, y, block_length, height) if horizontal else
                          (x, y + offset, width, block_length))
            offset += block_length
    blocks.extend((x - 9, y - 9, 18, 18) for x, y, _ in visual_joints)
    return blocks


def visual_wall_block_count(level: dict[str, object]) -> int:
    return len(visual_wall_blocks(level))


def validate_visual_wall_topology(level: dict[str, object], level_number: int) -> None:
    visual_walls, visual_joints = visual_wall_topology(level)
    rectangles = [(wall[0], wall[1], wall[0] + wall[2], wall[1] + wall[3], "wall")
                  for wall in visual_walls]
    rectangles.extend((x - 9, y - 9, x + 9, y + 9, "joint") for x, y, _ in visual_joints)
    for first_index, first in enumerate(rectangles):
        for second in rectangles[first_index + 1:]:
            overlap_width = min(first[2], second[2]) - max(first[0], second[0])
            overlap_height = min(first[3], second[3]) - max(first[1], second[1])
            if overlap_width > 0 and overlap_height > 0:
                raise ValueError(f"level {level_number}: visual {first[4]} overlaps visual {second[4]}")


def validate_visual_wall_coverage(level: dict[str, object], level_number: int) -> None:
    covered: set[tuple[int, int]] = set()
    for x, y, width, height in visual_wall_blocks(level):
        covered.update((pixel_x, pixel_y)
                       for pixel_x in range(x, x + width)
                       for pixel_y in range(y, y + height))
    for wall in level.get("walls", []):
        x, y, width, height = wall
        covered_pixels = 0
        for pixel_x in range(x, x + width):
            for pixel_y in range(y, y + height):
                covered_pixels += int((pixel_x, pixel_y) in covered)
        if covered_pixels * 2 < width * height:
            raise ValueError(f"level {level_number}: collision wall {wall} is mostly invisible")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-only", action="store_true", help="validate levels and emit only the C++ level header")
    args = parser.parse_args()
    manifest = json.loads(LEVELS_PATH.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1 or not manifest.get("levels"):
        raise SystemExit("levels.json must contain schema_version 1 and at least one level")
    levels = manifest["levels"]
    for level_number, level in enumerate(levels, start=1):
        if len(level.get("walls", [])) > 160:
            raise ValueError(f"level {level_number}: at most 160 wall tiles are supported")
        visual_walls, visual_joints = visual_wall_topology(level)
        if len(visual_walls) > 128:
            raise ValueError(f"level {level_number}: at most 128 visual wall segments are supported")
        if len(visual_joints) > 64:
            raise ValueError(f"level {level_number}: at most 64 wall joints are supported")
        if visual_wall_block_count(level) > 172:
            raise ValueError(f"level {level_number}: at most 172 visual wall blocks are supported")
        validate_visual_wall_topology(level, level_number)
        validate_visual_wall_coverage(level, level_number)
        if len(level.get("fans", [])) > 2:
            raise ValueError(f"level {level_number}: at most two fans are supported")
        for raw_fan in level.get("fans", []):
            fan = normalized_fan(raw_fan)
            if fan[5] != 0 and (fan[6] == 0 or fan[6] > fan[5]):
                raise ValueError(f"level {level_number}: pulsed fan active_ms must be within its period")
        validate_level_routes(level, level_number)
        validate_level_challenges(level, level_number)
    if args.data_only:
        emit_header(levels)
        return
    render_board_base(600).save(ASSETS_DIR / "board_base_720.png", optimize=True)
    render_board_base(400).save(ASSETS_DIR / "board_base_480.png", optimize=True)
    render_board_frame(600).save(ASSETS_DIR / "board_frame_720.png", optimize=True)
    render_board_frame(400).save(ASSETS_DIR / "board_frame_480.png", optimize=True)
    render_board_tiles(96).save(ASSETS_DIR / "board_tiles_720.png", optimize=True)
    render_board_tiles(64).save(ASSETS_DIR / "board_tiles_480.png", optimize=True)
    render_objects(96).save(ASSETS_DIR / "objects_720.png", optimize=True)
    render_objects(64).save(ASSETS_DIR / "objects_480.png", optimize=True)
    render_fans(96).save(ASSETS_DIR / "fans_720.png", optimize=True)
    render_fans(64).save(ASSETS_DIR / "fans_480.png", optimize=True)
    render_mechanics(96).save(ASSETS_DIR / "mechanics_720.png", optimize=True)
    render_mechanics(64).save(ASSETS_DIR / "mechanics_480.png", optimize=True)
    render_title(240, 60).save(ASSETS_DIR / "title_720.png", optimize=True)
    render_title(160, 40).save(ASSETS_DIR / "title_480.png", optimize=True)
    render_hud_icons(60).save(ASSETS_DIR / "hud_icons_720.png", optimize=True)
    render_hud_icons(40).save(ASSETS_DIR / "hud_icons_480.png", optimize=True)
    for stale_board in ASSETS_DIR.glob("board_level_*_*.png"):
        stale_board.unlink()
    if (SOURCE_DIR / "board-layouts.sha256").exists():
        (SOURCE_DIR / "board-layouts.sha256").unlink()
    emit_header(levels)


if __name__ == "__main__":
    main()
