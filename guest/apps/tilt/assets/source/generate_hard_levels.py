#!/usr/bin/env python3
"""Generate deterministic, physics-qualified Juicy Tilt level catalogs."""

from __future__ import annotations

import argparse
import itertools
import json
import math
import random
import subprocess
import tempfile
from collections import Counter, deque
from pathlib import Path

from generate_gameplay_assets import emit_header, route_hits_circle, validate_level_challenges, validate_level_routes


SOURCE_DIR = Path(__file__).resolve().parent
LEVELS_PATH = SOURCE_DIR / "levels.json"
GRID_SIZE = 7
CELL_SIZE = 78
ORIGIN = 27
WALL_THICKNESS = 18
PLAYER_MAX_TILT_ACCELERATION = 820
PULSED_FAN_MINIMUM_SCALE = 0.94
PULSED_FAN_FORCE = 1100
PULSED_FAN_MINIMUM_FORCE_MARGIN = 200
PULSED_FAN_PERIOD_MS = 3000
PULSED_FAN_ACTIVE_MS = 1400
PULSED_FAN_SECOND_PHASE_MS = 1500
MOVING_OBSTACLE_PERIOD_MS = 3000
TIMED_GATE_PERIOD_MS = 3600
TIMED_GATE_OPEN_MS = 1800
INFERNO_OPENING = 54
TIMING_CHOKE_GATE_PHASE_MS = 1500
INFERNO_STAGES = frozenset((3, 6, 9, 10))
EARLY_TIMING_CHOKE_LEVELS = frozenset((8, 10))
MINIMUM_TARGET_SPACING_STEPS = 4
MINIMUM_VISUAL_TARGET_SPACING_CELLS = 2
MINIMUM_PORTAL_BACKTRACK_STEPS = 4
MINIMUM_PORTAL_VISUAL_DISTANCE_CELLS = 3
MINIMUM_PORTAL_EDGE_CLEARANCE_CELLS = 2
PORTAL_ENTRANCE_RADIUS = 24
PORTAL_ROUTE_MARGIN = 3
MINIMUM_PORTAL_BYPASS_STEPS = 4
MINIMUM_HAZARD_DETOUR_STEPS = 2
UNAVOIDABLE_HAZARD_DETOUR_STEPS = GRID_SIZE * 2
DEFAULT_CANDIDATE_COUNT = 20
FINAL_LEVEL_CANDIDATE_COUNT = 60
FIRST_GENERATED_LEVEL = 2
APP_TOTAL_LEVEL_COUNT = 100
DEFAULT_GENERATED_LEVEL_COUNT = APP_TOTAL_LEVEL_COUNT - 1
DEFAULT_PHYSICS_BATCH_SIZE = 48
DEFAULT_SELECTION_BATCH_SIZE = 8
LEGACY_SEEDS = (2017, 3011, 4001, 5003, 6103, 7129, 8161, 9187, 10225, 11489, 12286, 13358)
INTRO_TARGET_DIFFICULTY_SCORES = (1050, 1280, 1500, 1700, 1950, 2160, 2280, 2380, 2500)
INTRO_TARGET_PHYSICS_TIME_MS = (28000, 35000, 41000, 47000, 53000, 59000,
                                65000, 70000, 76000)
MINIMUM_PHYSICS_TIME_STEP_MS = 500
CHAPTER_STAGE_TIME_OFFSETS_MS = (0, 2000, 4000, 6000, 1000, 8000, 10000, 12000, 14000, 18000)
CHAPTER_STAGE_SCORE_OFFSETS = (0, 40, 80, 120, 40, 150, 180, 210, 240, 300)
WORKSPACE_ROOT = SOURCE_DIR.parents[4]

Cell = tuple[int, int]
Edge = frozenset[Cell]


def campaign_position(number: int) -> tuple[int, int]:
    return (number - 1) // 10 + 1, (number - 1) % 10 + 1


def inferno_trial(number: int) -> bool:
    chapter, stage = campaign_position(number)
    return chapter == 10 and stage in INFERNO_STAGES


def timing_choke_trial(number: int) -> bool:
    if number in EARLY_TIMING_CHOKE_LEVELS or inferno_trial(number):
        return True
    _, stage = campaign_position(number)
    return number > 10 and stage == 6


def feature_profile(number: int) -> dict[str, object]:
    if number < FIRST_GENERATED_LEVEL:
        raise ValueError(f"generated level numbers must start at {FIRST_GENERATED_LEVEL}")
    if number <= 7:
        return {
            "name": f"progression-{number}",
            "second_fan": number >= 3,
            "moving": number >= 4,
            "timed_gate": number >= 5,
            "pressure_gate": number >= 6,
            "portal": number >= 7,
        }
    chapter, stage = campaign_position(number)
    if number == 8:
        pattern = ("first-combination", True, True, True, True, False)
    elif number == 9:
        pattern = ("first-portal-test", False, False, True, True, True)
    elif number == 10:
        pattern = ("chapter-exam", True, True, True, True, False)
    else:
        patterns = (
            ("route-recall", False, False, True, True, False),
            ("moving-wind", True, True, False, True, False),
            ("timing-chain", False, True, True, True, False),
            ("portal-choice", False, False, True, True, True),
            ("breather", False, False, True, True, False),
            ("crossfire", True, True, True, True, False),
            ("moving-portal", False, True, False, True, True),
            ("wind-portal", True, False, True, True, True),
            ("full-circuit", True, True, True, True, False),
            ("chapter-exam", stage == 10 and chapter == 10, True, True, True,
             stage == 10 and chapter % 2 == 0 and chapter < 10),
        )
        pattern = patterns[stage - 1]
    name, second_fan, moving, timed_gate, pressure_gate, portal = pattern
    return {
        "name": f"chapter-{chapter}-{name}",
        "chapter": chapter,
        "stage": stage,
        "exam": stage == 10,
        "breather": stage == 5,
        "inferno": inferno_trial(number),
        "timing_choke": timing_choke_trial(number),
        "second_fan": second_fan,
        "moving": moving,
        "timed_gate": timed_gate,
        "pressure_gate": pressure_gate,
        "portal": portal,
    }


def maze_loop_count(number: int, portal: bool) -> int:
    chapter, _ = campaign_position(number)
    if chapter <= 2:
        loops = 3
    elif chapter <= 4:
        loops = 2
    elif chapter <= 6:
        loops = 1
    else:
        loops = 0
    return max(2, loops) if portal else loops


def passage_opening(number: int, portal: bool) -> int:
    if portal:
        return 70
    if inferno_trial(number):
        return INFERNO_OPENING
    if timing_choke_trial(number):
        if number == 8:
            return 62
        if number == 10:
            return 60
        chapter, _ = campaign_position(number)
        return max(INFERNO_OPENING, 63 - chapter)
    if number <= 7:
        return max(64, 72 - (number - FIRST_GENERATED_LEVEL) * 2)
    chapter, _ = campaign_position(number)
    return (68, 68, 66, 66, 64, 64, 62, 62, 60, 60)[chapter - 1]


def minimum_pressure_detour_steps(number: int) -> int:
    chapter, _ = campaign_position(number)
    return 4 + min(3, (chapter - 1) // 3) * 2


def minimum_portal_backtrack_steps(number: int) -> int:
    chapter, _ = campaign_position(number)
    return 4 + (chapter - 1) // 2


def center(cell: Cell) -> tuple[int, int]:
    row, column = cell
    return ORIGIN + column * CELL_SIZE + CELL_SIZE // 2, ORIGIN + row * CELL_SIZE + CELL_SIZE // 2


def neighbors(cell: Cell) -> list[Cell]:
    row, column = cell
    result = []
    for dr, dc in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        candidate = row + dr, column + dc
        if 0 <= candidate[0] < GRID_SIZE and 0 <= candidate[1] < GRID_SIZE:
            result.append(candidate)
    return result


def carve(seed: int, start: Cell, loop_count: int = 2) -> set[Edge]:
    randomizer = random.Random(seed)
    visited = {start}
    stack = [start]
    passages: set[Edge] = set()
    while stack:
        current = stack[-1]
        options = [candidate for candidate in neighbors(current) if candidate not in visited]
        if not options:
            stack.pop()
            continue
        candidate = randomizer.choice(options)
        passages.add(frozenset((current, candidate)))
        visited.add(candidate)
        stack.append(candidate)

    # Early chapters add more alternate routes. Later chapters remove loops so
    # planning errors and missed stars require longer, but still fully open,
    # backtracking routes.
    candidates = []
    for row in range(GRID_SIZE):
        for column in range(GRID_SIZE):
            cell = row, column
            for candidate in neighbors(cell):
                edge = frozenset((cell, candidate))
                if edge not in passages and edge not in candidates:
                    candidates.append(edge)
    randomizer.shuffle(candidates)
    passages.update(candidates[:loop_count])
    return passages


def distances(passages: set[Edge], start: Cell) -> dict[Cell, int]:
    result = {start: 0}
    pending = deque([start])
    while pending:
        cell = pending.popleft()
        for candidate in neighbors(cell):
            if frozenset((cell, candidate)) not in passages or candidate in result:
                continue
            result[candidate] = result[cell] + 1
            pending.append(candidate)
    return result


def distances_without_edge(passages: set[Edge], start: Cell, blocked_edge: Edge) -> dict[Cell, int]:
    result = {start: 0}
    pending = deque([start])
    while pending:
        cell = pending.popleft()
        for candidate in neighbors(cell):
            edge = frozenset((cell, candidate))
            if edge == blocked_edge or edge not in passages or candidate in result:
                continue
            result[candidate] = result[cell] + 1
            pending.append(candidate)
    return result


def reachable_without_edge(passages: set[Edge], start: Cell, blocked_edge: Edge) -> set[Cell]:
    return set(distances_without_edge(passages, start, blocked_edge))


def path_between(passages: set[Edge], start: Cell, goal: Cell) -> list[Cell]:
    previous: dict[Cell, Cell | None] = {start: None}
    pending = deque([start])
    while pending:
        cell = pending.popleft()
        if cell == goal:
            break
        for candidate in neighbors(cell):
            if frozenset((cell, candidate)) not in passages or candidate in previous:
                continue
            previous[candidate] = cell
            pending.append(candidate)
    path = []
    cursor: Cell | None = goal
    while cursor is not None:
        path.append(cursor)
        cursor = previous[cursor]
    return list(reversed(path))


def path_between_avoiding(passages: set[Edge], start: Cell, goal: Cell,
                          blocked: Cell) -> list[Cell] | None:
    previous: dict[Cell, Cell | None] = {start: None}
    pending = deque([start])
    while pending:
        cell = pending.popleft()
        if cell == goal:
            break
        for candidate in neighbors(cell):
            if (candidate == blocked or frozenset((cell, candidate)) not in passages or
                    candidate in previous):
                continue
            previous[candidate] = cell
            pending.append(candidate)
    if goal not in previous:
        return None
    path = []
    cursor: Cell | None = goal
    while cursor is not None:
        path.append(cursor)
        cursor = previous[cursor]
    return list(reversed(path))


def route_steps(passages: set[Edge], checkpoints: tuple[Cell, ...]) -> int:
    return sum(len(path_between(passages, first, second)) - 1
               for first, second in zip(checkpoints, checkpoints[1:]))


def joined_route(passages: set[Edge], checkpoints: tuple[Cell, ...]) -> list[Cell]:
    result = [checkpoints[0]]
    for first, second in zip(checkpoints, checkpoints[1:]):
        result.extend(path_between(passages, first, second)[1:])
    return result


def all_star_route(passages: set[Edge], start: Cell, goal: Cell, stars: list[Cell],
                   blocked_cells: frozenset[Cell] = frozenset(),
                   blocked_edges: frozenset[Edge] = frozenset()) -> list[Cell] | None:
    """Return a shortest all-star route while treating selected hazards as impassable."""
    if start in blocked_cells or goal in blocked_cells or any(star in blocked_cells for star in stars):
        return None
    star_bits = {cell: 1 << index for index, cell in enumerate(stars)}
    all_stars = (1 << len(stars)) - 1
    start_state = (start, star_bits.get(start, 0))
    previous: dict[tuple[Cell, int], tuple[Cell, int] | None] = {start_state: None}
    pending = deque([start_state])
    final_state = None
    while pending:
        state = pending.popleft()
        cell, star_mask = state
        if cell == goal and star_mask == all_stars:
            final_state = state
            break
        for candidate in neighbors(cell):
            edge = frozenset((cell, candidate))
            if (candidate in blocked_cells or edge in blocked_edges or edge not in passages):
                continue
            next_state = (candidate, star_mask | star_bits.get(candidate, 0))
            if next_state in previous:
                continue
            previous[next_state] = state
            pending.append(next_state)
    if final_state is None:
        return None
    route = []
    cursor = final_state
    while cursor is not None:
        route.append(cursor[0])
        cursor = previous[cursor]
    return list(reversed(route))


def avoidance_detour(passages: set[Edge], start: Cell, goal: Cell, stars: list[Cell],
                      baseline_steps: int, blocked_cells: frozenset[Cell] = frozenset(),
                      blocked_edges: frozenset[Edge] = frozenset()) -> tuple[int, bool]:
    alternative = all_star_route(passages, start, goal, stars, blocked_cells, blocked_edges)
    if alternative is None:
        return UNAVOIDABLE_HAZARD_DETOUR_STEPS, True
    return max(0, len(alternative) - 1 - baseline_steps), False


def state_route(passages: set[Edge], start: Cell, goal: Cell, stars: list[Cell], switch: Cell | None,
                gate_edge: Edge | None, portals: tuple[Cell, Cell] | None) -> list[Cell]:
    """BFS over position, collected stars, and the latched pressure-gate state."""
    star_bits = {cell: 1 << index for index, cell in enumerate(stars)}
    all_stars = (1 << len(stars)) - 1
    start_state = (start, star_bits.get(start, 0), switch is not None and start == switch)
    previous: dict[tuple[Cell, int, bool], tuple[tuple[Cell, int, bool], tuple[Cell, ...]] | None] = {
        start_state: None
    }
    pending = deque([start_state])
    portal_map = {} if portals is None else {portals[0]: portals[1]}
    final_state = None
    while pending:
        state = pending.popleft()
        cell, star_mask, switch_on = state
        if cell == goal and star_mask == all_stars:
            final_state = state
            break
        for candidate in neighbors(cell):
            edge = frozenset((cell, candidate))
            if edge not in passages or (edge == gate_edge and not switch_on):
                continue
            transition = [candidate]
            destination = candidate
            next_switch = switch_on or (switch is not None and candidate == switch)
            next_stars = star_mask | star_bits.get(candidate, 0)
            if candidate in portal_map:
                destination = portal_map[candidate]
                transition.append(destination)
                next_stars |= star_bits.get(destination, 0)
            next_state = (destination, next_stars, next_switch)
            if next_state in previous:
                continue
            previous[next_state] = (state, tuple(transition))
            pending.append(next_state)
    if final_state is None:
        raise ValueError("state-space route has no all-star solution")
    transitions = []
    cursor = final_state
    while previous[cursor] is not None:
        prior, transition = previous[cursor]
        transitions.append(transition)
        cursor = prior
    result = [start]
    for transition in reversed(transitions):
        result.extend(transition)
    return result


def choose_pressure_gate(passages: set[Edge], start: Cell, goal: Cell,
                         star_cells: list[Cell], natural_route: list[Cell],
                         excluded_edges: set[Edge], minimum_detour_steps: int = 4) -> tuple[Edge, Cell, int]:
    natural_route_cells = set(natural_route)
    best = None
    for edge in passages:
        if edge in excluded_edges:
            continue
        distance_from_start = distances_without_edge(passages, start, edge)
        component = set(distance_from_start)
        if len(component) == GRID_SIZE * GRID_SIZE:
            continue
        outside_targets = int(goal not in component) + sum(star not in component for star in star_cells)
        if outside_targets == 0:
            continue
        gate_entries = [cell for cell in edge if cell in component]
        if len(gate_entries) != 1:
            continue
        gate_entry = gate_entries[0]
        switch_candidates = [cell for cell in component
                             if cell not in (start, goal, *star_cells) and cell not in edge and
                             cell not in natural_route_cells and
                             sum(frozenset((cell, candidate)) in passages for candidate in neighbors(cell)) == 1]
        if not switch_candidates:
            continue
        for switch_cell in switch_candidates:
            distance_from_switch = distances_without_edge(passages, switch_cell, edge)
            detour_steps = (distance_from_start[switch_cell] + distance_from_switch[gate_entry] -
                            distance_from_start[gate_entry])
            # A switch on the direct route has a zero-step detour. Requiring four
            # extra edges guarantees at least a two-cell branch and a real
            # out-and-back decision before the player can cross the gate.
            if detour_steps < minimum_detour_steps:
                continue
            score = (outside_targets * 100 + detour_steps * 30 +
                     distance_from_start[switch_cell] * 5 - len(component))
            if best is None or score > best[0]:
                best = score, edge, switch_cell, detour_steps
    if best is None:
        raise ValueError(f"maze has no pressure gate with a {minimum_detour_steps}-step switch detour")
    return best[1], best[2], best[3]


def choose_portal_trap(passages: set[Edge], route: list[Cell],
                       protected: set[Cell],
                       minimum_backtrack_steps: int = MINIMUM_PORTAL_BACKTRACK_STEPS
                       ) -> tuple[Cell, Cell, int, int, list[Cell]] | None:
    """Block the shortest route with a trap while preserving a clear grid-cell bypass."""
    best = None
    for entrance_index in range(5, len(route) - 1):
        entrance = route[entrance_index]
        if (entrance in protected or route.count(entrance) != 1 or
                route[entrance_index - 1] == route[entrance_index + 1]):
            continue
        centrality = min(entrance[0], entrance[1], GRID_SIZE - 1 - entrance[0],
                         GRID_SIZE - 1 - entrance[1])
        if centrality < MINIMUM_PORTAL_EDGE_CLEARANCE_CELLS:
            continue
        bypass = path_between_avoiding(
            passages, route[entrance_index - 1], route[entrance_index + 1], entrance)
        if bypass is None or len(bypass) - 1 < MINIMUM_PORTAL_BYPASS_STEPS:
            continue
        for destination_index, destination in enumerate(route[:entrance_index - 4]):
            if destination in protected or route.count(destination) != 1:
                continue
            backtrack_steps = distances(passages, destination)[entrance]
            visual_distance = (abs(entrance[0] - destination[0]) +
                               abs(entrance[1] - destination[1]))
            if (backtrack_steps < minimum_backtrack_steps or
                    visual_distance < MINIMUM_PORTAL_VISUAL_DISTANCE_CELLS):
                continue
            score = (centrality, backtrack_steps, len(bypass) - 1,
                     entrance_index - destination_index, visual_distance, entrance_index)
            if best is None or score > best[0]:
                best = score, entrance, destination, entrance_index, backtrack_steps, bypass
    return None if best is None else (best[1], best[2], best[3], best[4], best[5])


def difficulty_screen(level: dict[str, object], passages: set[Edge], start: Cell, goal: Cell,
                      main_path: list[Cell], star_cells: list[Cell], opening: int,
                      number: int) -> dict[str, int]:
    """Conservative proxy for ball clearance, route effort, and dynamic timing."""
    clearance = opening - 36
    timing_choke = bool(level["generator"].get("timing_choke"))
    inferno = bool(level["generator"].get("inferno_trial"))
    if clearance < (18 if timing_choke else 22):
        raise ValueError(f"level {number}: only {clearance}px ball clearance")

    solution_points = level.get("solution", [])
    mastery_cells = [
        (round((point[1] - ORIGIN - CELL_SIZE // 2) / CELL_SIZE),
         round((point[0] - ORIGIN - CELL_SIZE // 2) / CELL_SIZE))
        for point in solution_points
    ]
    mastery_steps = max(0, len(mastery_cells) - 1)
    direct_steps = len(main_path) - 1
    turns = 0
    previous_direction = None
    for first, second in zip(mastery_cells, mastery_cells[1:]):
        direction = second[0] - first[0], second[1] - first[1]
        if previous_direction is not None and direction != previous_direction:
            turns += 1
        previous_direction = direction

    expected_wait_ms = 0
    fans = level["fans"]
    for fan in fans:
        _, _, _, force_x, force_y, period, active, _ = fan
        if period != 0 and (active >= period or period - active < 600):
            raise ValueError(f"level {number}: fan has no useful recovery window")
        minimum_force = math.hypot(force_x, force_y) * PULSED_FAN_MINIMUM_SCALE
        if period != 0 and minimum_force < PLAYER_MAX_TILT_ACCELERATION + PULSED_FAN_MINIMUM_FORCE_MARGIN:
            raise ValueError(f"level {number}: pulsed fan lacks a decisive force margin")
        if period != 0:
            expected_wait_ms += active * active // (2 * period)
    gate = level["gate"]
    if gate[4] and (gate[5] * 100 // gate[4] < 30):
        raise ValueError(f"level {number}: gate open window is too short")
    if gate[4]:
        closed_ms = gate[4] - gate[5]
        expected_wait_ms += closed_ms * closed_ms // (2 * gate[4])
    moving = level["moving_wall"]
    if moving[6] and abs(moving[4] - moving[0]) + abs(moving[5] - moving[1]) < 42:
        raise ValueError(f"level {number}: blocker does not retract far enough")
    if moving[6]:
        expected_wait_ms += moving[6] // 6

    pressure_gate = level["pressure_gate"]
    portals = level["portals"]
    switch_detour = int(level["generator"].get("pressure_switch_detour_steps", 0))
    portal_bypass_extra = int(level["generator"].get("portal_bypass_extra_steps", 0))
    if pressure_gate[2] != 0 and switch_detour < 4:
        raise ValueError(f"level {number}: pressure switch does not require a meaningful detour")
    hazards = (len(fans) + int(gate[4] != 0) + int(moving[6] != 0) +
               int(pressure_gate[2] != 0) + int(portals[2] != 0))
    criticality = level["generator"].get("hazard_criticality", {})
    hazard_detour_steps = sum(min(UNAVOIDABLE_HAZARD_DETOUR_STEPS, int(value["detour_steps"]))
                              for value in criticality.values())
    unavoidable_hazards = sum(bool(value["unavoidable"]) for value in criticality.values())
    # The proxy now prices the authored all-star state route, expected timing
    # waits, and the cost of avoiding each hazard. Merely adding an optional
    # object therefore contributes little compared with a true choke point.
    choke_wait_ms = 1800 if inferno else 1000 if timing_choke else 0
    proxy_time_ms = mastery_steps * 1000 + turns * 300 + expected_wait_ms + hazards * 450 + choke_wait_ms
    score = (mastery_steps * 13 + turns * 15 + hazards * 35 + hazard_detour_steps * 12 +
             unavoidable_hazards * 30 + expected_wait_ms // 80 + (74 - opening) * 6 +
             (180 if inferno else 100 if timing_choke else 0))
    if score < 260:
        raise ValueError(f"level {number}: generated challenge score {score} is too low")
    mastery_par_ms = ((proxy_time_ms * 118 // 100 + 999) // 1000) * 1000
    level["par_time_ms"] = mastery_par_ms
    return {
        "direct_steps": direct_steps,
        "mastery_steps": mastery_steps,
        "turns": turns,
        "clearance": clearance,
        "dynamic_hazards": hazards,
        "pressure_switch_detour_steps": switch_detour,
        "portal_bypass_extra_steps": portal_bypass_extra,
        "hazard_detour_steps": hazard_detour_steps,
        "unavoidable_hazards": unavoidable_hazards,
        "expected_wait_ms": expected_wait_ms,
        "proxy_time_ms": proxy_time_ms,
        "mastery_par_ms": mastery_par_ms,
        "score": score,
    }


def walls_from_passages(passages: set[Edge], opening: int) -> list[list[int]]:
    walls: list[list[int]] = []
    stub = (CELL_SIZE - opening) // 2
    for row in range(GRID_SIZE):
        for column in range(GRID_SIZE - 1):
            left = row, column
            right = row, column + 1
            x = ORIGIN + (column + 1) * CELL_SIZE - WALL_THICKNESS // 2
            y = ORIGIN + row * CELL_SIZE
            if frozenset((left, right)) in passages:
                if stub > 0:
                    walls.extend(([x, y, WALL_THICKNESS, stub],
                                  [x, y + CELL_SIZE - stub, WALL_THICKNESS, stub]))
            else:
                walls.append([x, y, WALL_THICKNESS, CELL_SIZE])
    for row in range(GRID_SIZE - 1):
        for column in range(GRID_SIZE):
            top = row, column
            bottom = row + 1, column
            x = ORIGIN + column * CELL_SIZE
            y = ORIGIN + (row + 1) * CELL_SIZE - WALL_THICKNESS // 2
            if frozenset((top, bottom)) in passages:
                if stub > 0:
                    walls.extend(([x, y, stub, WALL_THICKNESS],
                                  [x + CELL_SIZE - stub, y, stub, WALL_THICKNESS]))
            else:
                walls.append([x, y, CELL_SIZE, WALL_THICKNESS])
    return walls


def gate_for_edge(first: Cell, second: Cell, opening: int, period: int, open_ms: int, phase: int) -> list[int]:
    first_x, first_y = center(first)
    second_x, second_y = center(second)
    if first_x != second_x:
        boundary_x = (first_x + second_x) // 2
        return [boundary_x - WALL_THICKNESS // 2, first_y - opening // 2,
                WALL_THICKNESS, opening, period, open_ms, phase]
    boundary_y = (first_y + second_y) // 2
    return [first_x - opening // 2, boundary_y - WALL_THICKNESS // 2,
            opening, WALL_THICKNESS, period, open_ms, phase]


def blocker_for_edge(first: Cell, second: Cell, opening: int, period: int) -> list[int]:
    first_x, first_y = center(first)
    second_x, second_y = center(second)
    if first_x != second_x:
        boundary_x = (first_x + second_x) // 2
        start_y = first_y - opening // 2
        return [boundary_x - WALL_THICKNESS // 2, start_y, WALL_THICKNESS, opening,
                boundary_x - WALL_THICKNESS // 2, start_y + opening, period]
    boundary_y = (first_y + second_y) // 2
    start_x = first_x - opening // 2
    return [start_x, boundary_y - WALL_THICKNESS // 2, opening, WALL_THICKNESS,
            start_x + opening, boundary_y - WALL_THICKNESS // 2, period]


def opposing_force(first: Cell, second: Cell, strength: int) -> tuple[int, int]:
    first_x, first_y = center(first)
    second_x, second_y = center(second)
    dx = second_x - first_x
    dy = second_y - first_y
    return (-strength if dx > 0 else strength if dx < 0 else 0,
            -strength if dy > 0 else strength if dy < 0 else 0)


def choose_feature_index(route: list[Cell], fraction: float, protected: set[Cell],
                         excluded: set[Cell] | None = None) -> int:
    excluded = excluded or set()
    preferred = round((len(route) - 1) * fraction)
    candidates = []
    for index in range(1, len(route) - 1):
        cell = route[index]
        if cell in protected or cell in excluded or route[index - 1] == cell:
            continue
        # A fan frame is wider than one grid cell. Keep two cell centers of
        # visual clearance from stars/start/goal so gameplay objects never
        # disappear beneath the fan art.
        if any((cell[0] - target[0]) ** 2 + (cell[1] - target[1]) ** 2 < 4 for target in protected):
            continue
        if any((cell[0] - target[0]) ** 2 + (cell[1] - target[1]) ** 2 < 4 for target in excluded):
            continue
        candidates.append(index)
    if not candidates:
        raise ValueError("route has no visually separated feature cell")
    return min(candidates, key=lambda index: abs(index - preferred))


def choose_critical_cell(passages: set[Edge], route: list[Cell], fraction: float, start: Cell, goal: Cell,
                         stars: list[Cell], protected: set[Cell], excluded: set[Cell] | None = None,
                         minimum_visual_distance_squared: int = 4) -> tuple[int, int, bool]:
    """Choose a route cell whose removal forces a meaningful all-star detour."""
    excluded = excluded or set()
    preferred = round((len(route) - 1) * fraction)
    baseline_steps = len(all_star_route(passages, start, goal, stars) or []) - 1
    best = None
    for index in range(1, len(route) - 1):
        cell = route[index]
        if cell in protected or cell in excluded or route.count(cell) != 1:
            continue
        if any((cell[0] - target[0]) ** 2 + (cell[1] - target[1]) ** 2 < minimum_visual_distance_squared
               for target in protected | excluded):
            continue
        detour, unavoidable = avoidance_detour(
            passages, start, goal, stars, baseline_steps, blocked_cells=frozenset((cell,)))
        score = (int(unavoidable), detour, -abs(index - preferred), -index)
        if best is None or score > best[0]:
            best = score, index, detour, unavoidable
    if best is None or best[2] < MINIMUM_HAZARD_DETOUR_STEPS:
        raise ValueError("route has no strategically critical hazard cell")
    return best[1], best[2], best[3]


def choose_critical_edge(passages: set[Edge], route: list[Cell], fraction: float, start: Cell, goal: Cell,
                         stars: list[Cell], excluded_edges: set[Edge],
                         excluded_cells: set[Cell]) -> tuple[int, Edge, int, bool]:
    """Choose a route edge that cannot be bypassed cheaply by another all-star route."""
    preferred = round((len(route) - 1) * fraction)
    baseline_steps = len(all_star_route(passages, start, goal, stars) or []) - 1
    best = None
    for index in range(1, len(route)):
        edge = frozenset((route[index - 1], route[index]))
        if (len(edge) != 2 or edge in excluded_edges or
                any(cell in excluded_cells for cell in edge) or
                sum(1 for route_index in range(1, len(route))
                    if frozenset((route[route_index - 1], route[route_index])) == edge) != 1):
            continue
        detour, unavoidable = avoidance_detour(
            passages, start, goal, stars, baseline_steps, blocked_edges=frozenset((edge,)))
        score = (int(unavoidable), detour, -abs(index - preferred), -index)
        if best is None or score > best[0]:
            best = score, index, edge, detour, unavoidable
    if best is None or best[3] < MINIMUM_HAZARD_DETOUR_STEPS:
        raise ValueError("route has no strategically critical hazard edge")
    return best[1], best[2], best[3], best[4]


def choose_star_cells(passages: set[Edge], start: Cell, goal: Cell, main_path: list[Cell]) -> list[Cell]:
    cells = list(distances(passages, start))
    degree = {cell: sum(frozenset((cell, candidate)) in passages for candidate in neighbors(cell))
              for cell in cells}
    all_distances = {cell: distances(passages, cell) for cell in cells}
    best = None
    for stars in itertools.combinations((cell for cell in cells if cell not in (start, goal)), 3):
        targets = (start, goal, *stars)
        spacing = min(all_distances[first][second]
                      for index, first in enumerate(targets) for second in targets[index + 1:])
        visible_targets = (goal, *stars)
        visual_spacing = min(abs(first[0] - second[0]) + abs(first[1] - second[1])
                             for index, first in enumerate(visible_targets)
                             for second in visible_targets[index + 1:])
        if (spacing < MINIMUM_TARGET_SPACING_STEPS or
                visual_spacing < MINIMUM_VISUAL_TARGET_SPACING_CELLS):
            continue
        mastery_steps = min(sum(all_distances[first][second]
                                for first, second in zip((start, *order), (*order, goal)))
                            for order in itertools.permutations(stars))
        leaf_count = sum(degree[cell] == 1 for cell in stars)
        off_main_path = sum(cell not in main_path for cell in stars)
        score = (spacing, mastery_steps, leaf_count, off_main_path,
                 sum(all_distances[start][cell] for cell in stars))
        if best is None or score > best[0]:
            best = score, stars
    if best is None:
        raise ValueError("maze cannot separate start, stars, and goal by four route steps")
    return list(best[1])


def build_level(number: int, seed: int) -> dict[str, object]:
    ordinal = number - FIRST_GENERATED_LEVEL
    profile = feature_profile(number)
    chapter, stage = campaign_position(number)
    start_cell = (5, 0) if number % 2 == 0 else (0, 0)
    loop_count = maze_loop_count(number, bool(profile["portal"]))
    passages = carve(seed, start_cell, loop_count)
    distance = distances(passages, start_cell)
    goal_cell = max(distance, key=distance.get)
    main_path = path_between(passages, start_cell, goal_cell)
    star_cells = choose_star_cells(passages, start_cell, goal_cell, main_path)
    required_stars = 3
    clear_path = all_star_route(passages, start_cell, goal_cell, star_cells)
    if clear_path is None:
        raise ValueError("maze has no all-star route")
    # Every portal level gives the ball 34 px of total lateral room at every
    # passage. A bypass must feel like a route, not a seam that is merely
    # non-zero in the collision model.
    opening = passage_opening(number, bool(profile["portal"]))
    walls = walls_from_passages(passages, opening)

    protected_feature_cells = {start_cell, goal_cell, *star_cells}
    ice_index, ice_detour, ice_unavoidable = choose_critical_cell(
        passages, clear_path, 0.28, start_cell, goal_cell, star_cells,
        protected_feature_cells, minimum_visual_distance_squared=2)
    ice_cell = clear_path[ice_index]
    ice_x, ice_y = center(ice_cell)
    occupied_hazard_cells = {ice_cell}
    hazard_criticality: dict[str, dict[str, int | bool]] = {
        "ice": {"detour_steps": ice_detour, "unavoidable": ice_unavoidable}
    }

    level: dict[str, object] = {
        "required_stars": required_stars,
        "par_time_ms": 30000 + min(ordinal, 11) * 3000 + len(main_path) * 900,
        "start": list(center(start_cell)),
        "goal": [*center(goal_cell), 34],
        "pit": [0, 0, 0],
        "bumper": [0, 0, 0],
        "bumper_end": [0, 0],
        "bumper_period_ms": 0,
        "ice": [ice_x - 31, ice_y - 31, 62, 62],
        "stars": [list(center(cell)) for cell in star_cells],
        "fans": [],
        "moving_wall": [0, 0, 0, 0, 0, 0, 0],
        "gate": [0, 0, 0, 0, 0, 0, 0],
        "pressure_gate": [0, 0, 0, 0, 0, 0, 0],
        "portals": [0, 0, 0, 0, 0, 0],
        "walls": walls,
        "solution": [list(center(cell)) for cell in clear_path],
        "generator": {
            "seed": seed,
            "opening": opening,
            "chapter": chapter,
            "stage": stage,
            "chapter_exam": stage == 10,
            "breather": stage == 5,
            "inferno_trial": bool(profile.get("inferno")),
            "timing_choke_trial": bool(profile.get("timing_choke")),
            "grid_size": GRID_SIZE,
            "cell_size": CELL_SIZE,
            "origin": ORIGIN,
            "maze_loop_count": loop_count,
        },
    }

    challenge_index, fan_detour, fan_unavoidable = choose_critical_cell(
        passages, clear_path, 0.5, start_cell, goal_cell, star_cells,
        protected_feature_cells, occupied_hazard_cells)
    challenge_cell = clear_path[challenge_index]
    occupied_hazard_cells.add(challenge_cell)
    challenge_x, challenge_y = center(challenge_cell)
    previous = clear_path[challenge_index - 1]
    force_x, force_y = opposing_force(previous, challenge_cell, PULSED_FAN_FORCE)
    level["fans"] = [[challenge_x, challenge_y, 124, force_x, force_y,
                      PULSED_FAN_PERIOD_MS, PULSED_FAN_ACTIVE_MS, 0]]
    hazard_criticality["fan_1"] = {"detour_steps": fan_detour, "unavoidable": fan_unavoidable}
    second_cell = None
    if profile["second_fan"]:
        second_index, second_detour, second_unavoidable = choose_critical_cell(
            passages, clear_path, 2.0 / 3.0, start_cell, goal_cell, star_cells,
            protected_feature_cells, occupied_hazard_cells)
        second_cell = clear_path[second_index]
        occupied_hazard_cells.add(second_cell)
        second_x, second_y = center(second_cell)
        second_force = opposing_force(clear_path[second_index - 1], second_cell, PULSED_FAN_FORCE)
        level["fans"].append([second_x, second_y, 116, *second_force,
                              PULSED_FAN_PERIOD_MS, PULSED_FAN_ACTIVE_MS,
                              PULSED_FAN_SECOND_PHASE_MS])
        hazard_criticality["fan_2"] = {"detour_steps": second_detour, "unavoidable": second_unavoidable}
    used_dynamic_edges: set[Edge] = set()
    block_edge_cells: set[Cell] = set()
    if profile["moving"]:
        block_index, block_edge, block_detour, block_unavoidable = choose_critical_edge(
            passages, clear_path, 1.0 / 3.0, start_cell, goal_cell, star_cells,
            used_dynamic_edges, occupied_hazard_cells | protected_feature_cells)
        used_dynamic_edges.add(block_edge)
        block_edge_cells = set(block_edge)
        level["moving_wall"] = blocker_for_edge(clear_path[block_index - 1], clear_path[block_index], opening,
                                                MOVING_OBSTACLE_PERIOD_MS)
        hazard_criticality["moving_wall"] = {"detour_steps": block_detour, "unavoidable": block_unavoidable}
    timed_gate_cells: set[Cell] = set()
    if profile["timed_gate"]:
        gate_index, timed_edge, timed_detour, timed_unavoidable = choose_critical_edge(
            passages, clear_path, 3.0 / 4.0, start_cell, goal_cell, star_cells,
            used_dynamic_edges, occupied_hazard_cells | protected_feature_cells | block_edge_cells)
        used_dynamic_edges.add(timed_edge)
        timed_gate_cells = set(timed_edge)
        gate_phase = (TIMING_CHOKE_GATE_PHASE_MS if profile.get("timing_choke") else
                      ordinal * 700 % TIMED_GATE_PERIOD_MS)
        level["gate"] = gate_for_edge(clear_path[gate_index - 1], clear_path[gate_index], opening,
                                      TIMED_GATE_PERIOD_MS, TIMED_GATE_OPEN_MS, gate_phase)
        hazard_criticality["timed_gate"] = {"detour_steps": timed_detour, "unavoidable": timed_unavoidable}
        if profile.get("timing_choke"):
            fan_index = gate_index - 1
            if fan_index == 0:
                raise ValueError(f"level {number}: timing choke is too close to start")
            fan_cell = clear_path[fan_index]
            if any((fan_cell[0] - target[0]) ** 2 + (fan_cell[1] - target[1]) ** 2 < 4
                   for target in protected_feature_cells):
                raise ValueError(f"level {number}: timing choke overlaps a target")
            occupied_hazard_cells.discard(challenge_cell)
            occupied_hazard_cells.add(fan_cell)
            challenge_cell = fan_cell
            challenge_x, challenge_y = center(challenge_cell)
            force_x, force_y = opposing_force(clear_path[fan_index - 1], challenge_cell, PULSED_FAN_FORCE)
            level["fans"][0] = [challenge_x, challenge_y, 124, force_x, force_y,
                                PULSED_FAN_PERIOD_MS, PULSED_FAN_ACTIVE_MS, 0]
            baseline_steps = len(all_star_route(passages, start_cell, goal_cell, star_cells) or []) - 1
            fan_detour, fan_unavoidable = avoidance_detour(
                passages, start_cell, goal_cell, star_cells, baseline_steps,
                blocked_cells=frozenset((challenge_cell,)))
            if fan_detour < MINIMUM_HAZARD_DETOUR_STEPS:
                raise ValueError(f"level {number}: timing fan is not on a critical choke")
            hazard_criticality["fan_1"] = {"detour_steps": fan_detour, "unavoidable": fan_unavoidable}
            level["generator"]["timing_choke"] = {
                "tier": "inferno" if profile.get("inferno") else "skill",
                "fan_cell": list(challenge_cell),
                "gate_edge": [list(cell) for cell in sorted(timed_edge)],
                "opening": opening,
                "lateral_clearance_px": opening - 36,
                "gate_phase_ms": gate_phase,
            }
    gate_edge = None
    switch_cell = None
    switch_detour = 0
    portal_cells = None
    if profile["pressure_gate"]:
        gate_edge, switch_cell, switch_detour = choose_pressure_gate(
            passages, start_cell, goal_cell, star_cells, clear_path, used_dynamic_edges,
            minimum_pressure_detour_steps(number))
        used_dynamic_edges.add(gate_edge)
        gate_first, gate_second = sorted(gate_edge)
        gate_rect = gate_for_edge(gate_first, gate_second, opening, 0, 0, 0)[:4]
        switch_x, switch_y = center(switch_cell)
        level["pressure_gate"] = [switch_x, switch_y, 27, *gate_rect]
        route_without_portals = state_route(passages, start_cell, goal_cell, star_cells,
                                            switch_cell, gate_edge, None)
        state_solution = route_without_portals
        if profile["portal"]:
            fan_cells = {challenge_cell}
            if second_cell is not None:
                fan_cells.add(second_cell)
            hazard_cells = {ice_cell, *fan_cells, *block_edge_cells,
                            *timed_gate_cells, *gate_edge}
            portal_protected = {start_cell, goal_cell, switch_cell, *star_cells}
            portal_protected.update(hazard_cells)
            # Fan art and force fields need one full cell of breathing room.
            # Flat ice and edge-mounted doors may sit in a neighboring cell;
            # forbidding every neighbor over-constrained the late-game search.
            portal_protected.update({
                (row, column) for row in range(GRID_SIZE) for column in range(GRID_SIZE)
                if any(abs(row - hazard[0]) + abs(column - hazard[1]) <= 1
                       for hazard in fan_cells)
            })
            portal_trap = choose_portal_trap(
                passages, route_without_portals, portal_protected,
                minimum_portal_backtrack_steps(number))
            if portal_trap is None:
                raise ValueError(f"level {number}: no meaningful portal trap")
            (entrance_cell, destination_cell, entrance_index,
             portal_backtrack_steps, portal_bypass) = portal_trap
            portal_cells = entrance_cell, destination_cell
            first_x, first_y = center(entrance_cell)
            second_x, second_y = center(destination_cell)
            level["portals"] = [first_x, first_y, PORTAL_ENTRANCE_RADIUS,
                                second_x, second_y, PORTAL_ENTRANCE_RADIUS]
            solution_points = [list(center(cell)) for cell in state_solution]
            solution_points[entrance_index:entrance_index + 1] = [
                list(center(cell)) for cell in portal_bypass[1:-1]
            ]
            direct_route = [center(cell) for cell in state_solution]
            if not route_hits_circle(direct_route, first_x, first_y, PORTAL_ENTRANCE_RADIUS):
                raise ValueError(f"level {number}: portal trap does not punish the direct route")
            if route_hits_circle([tuple(point) for point in solution_points], first_x, first_y,
                                 PORTAL_ENTRANCE_RADIUS + PORTAL_ROUTE_MARGIN):
                raise ValueError(f"level {number}: portal trap leaves no safe control line")
        else:
            solution_points = [list(center(cell)) for cell in state_solution]
        level["solution"] = solution_points
        if profile["portal"]:
            bypass_points = [center(cell) for cell in portal_bypass]
            safe_fans = [
                (index, fan) for index, fan in enumerate(level["fans"])
                if not route_hits_circle(bypass_points, fan[0], fan[1], fan[2])
            ]
            if not safe_fans:
                raise ValueError(f"level {number}: every fan overlaps the portal bypass")
            # Keep only gusts that do not contaminate the portal decision.
            kept_fans = [fan for _, fan in safe_fans]
            kept_criticality = [hazard_criticality[f"fan_{index + 1}"]
                                for index, _ in safe_fans]
            level["fans"] = kept_fans
            hazard_criticality.pop("fan_1", None)
            hazard_criticality.pop("fan_2", None)
            for index, criticality in enumerate(kept_criticality, start=1):
                hazard_criticality[f"fan_{index}"] = criticality
            level["generator"]["portal_bypass_clearance_px"] = opening - 36
            level["generator"]["portal_bypass_avoids_fans"] = True
    if profile["pressure_gate"]:
        level["generator"]["pressure_gate_edge"] = [list(cell) for cell in sorted(gate_edge)]
        level["generator"]["pressure_switch_cell"] = list(switch_cell)
        level["generator"]["pressure_switch_detour_steps"] = switch_detour
        if portal_cells is not None:
            level["generator"]["portal_cells"] = [list(portal_cells[0]), list(portal_cells[1])]
            level["generator"]["portal_backtrack_steps"] = portal_backtrack_steps
            level["generator"]["portal_bypass_cells"] = [list(cell) for cell in portal_bypass]
            level["generator"]["portal_bypass_extra_steps"] = len(portal_bypass) - 3

    level["generator"]["hazard_criticality"] = hazard_criticality
    level["generator"]["profile"] = profile["name"]
    level["generator"]["topology_edges"] = [
        [list(cell) for cell in sorted(edge)] for edge in sorted(passages, key=lambda edge: sorted(edge))
    ]

    level["generator"]["difficulty"] = difficulty_screen(
        level, passages, start_cell, goal_cell, main_path, star_cells, opening, number)

    return level


def topology_edges(level: dict[str, object]) -> set[Edge]:
    return {
        frozenset(tuple(cell) for cell in raw_edge)
        for raw_edge in level["generator"]["topology_edges"]
    }


def topology_novelty(first: dict[str, object], others: list[dict[str, object]]) -> int:
    if not others:
        return 1000
    first_edges = topology_edges(first)
    scores = []
    for other in others:
        other_edges = topology_edges(other)
        union = first_edges | other_edges
        scores.append(1000 if not union else
                      1000 - len(first_edges & other_edges) * 1000 // len(union))
    return min(scores)


def transform_cell(cell: Cell, transform: int) -> Cell:
    """Apply one of the square's eight rotations/reflections."""
    row, column = cell
    if transform >= 4:
        column = GRID_SIZE - 1 - column
        transform -= 4
    for _ in range(transform):
        row, column = column, GRID_SIZE - 1 - row
    return row, column


def canonical_topology_signature(level: dict[str, object]) -> tuple[tuple[Cell, Cell], ...]:
    """Treat rotated and mirrored copies of a maze as the same topology."""
    signatures = []
    for transform in range(8):
        transformed = []
        for edge in topology_edges(level):
            first, second = sorted(transform_cell(cell, transform) for cell in edge)
            transformed.append((first, second))
        signatures.append(tuple(sorted(transformed)))
    return min(signatures)


def mechanic_signature(level: dict[str, object]) -> tuple[object, ...]:
    """Detect exact obstacle-layout repeats even when their timing differs."""
    return (
        tuple(tuple(fan[:5]) for fan in level["fans"]),
        tuple(level["ice"]),
        tuple(level["moving_wall"][:6]),
        tuple(level["gate"][:4]),
        tuple(level["pressure_gate"]),
        tuple(level["portals"]),
    )


def candidate_seed_stream(number: int):
    ordinal = number - FIRST_GENERATED_LEVEL
    legacy_seed = LEGACY_SEEDS[ordinal] if 0 <= ordinal < len(LEGACY_SEEDS) else None
    if legacy_seed is not None:
        yield legacy_seed
    randomizer = random.Random(0x4A55494359 + number)
    seen = set() if legacy_seed is None else {legacy_seed}
    while True:
        seed = randomizer.randrange(1, 2 ** 31)
        if seed in seen:
            continue
        seen.add(seed)
        yield seed


def generate_candidate_pool(number: int, candidate_count: int) -> tuple[list[dict[str, object]], int, Counter[str]]:
    candidates = []
    rejection_reasons: Counter[str] = Counter()
    maximum_attempts = max(40, candidate_count * 30)
    attempts = 0
    for seed in candidate_seed_stream(number):
        attempts += 1
        try:
            candidates.append(build_level(number, seed))
        except ValueError as error:
            rejection_reasons[str(error)] += 1
        if len(candidates) >= candidate_count or attempts >= maximum_attempts:
            break
    if len(candidates) < candidate_count:
        common = ", ".join(f"{reason} ({count})" for reason, count in rejection_reasons.most_common(3))
        raise ValueError(
            f"level {number}: only {len(candidates)}/{candidate_count} candidates survived {attempts} attempts; {common}")
    return candidates, attempts, rejection_reasons


def evaluate_candidate_physics(candidate_pools: list[list[dict[str, object]]], batch_size: int) -> None:
    """Run candidates through production physics in bounded compiler batches."""
    flattened = [candidate for pool in candidate_pools for candidate in pool]
    with tempfile.TemporaryDirectory(prefix="tilt-candidates-") as temporary:
        temporary_path = Path(temporary)
        header_path = temporary_path / "tilt_candidate_levels.hpp"
        executable_path = temporary_path / "tilt_candidate_evaluator"
        for offset in range(0, len(flattened), batch_size):
            batch = flattened[offset:offset + batch_size]
            emit_header(batch, header_path)
            define = '-DMICROPIXEL_TILT_LEVEL_DATA_HEADER="tilt_candidate_levels.hpp"'
            compile_command = [
                "clang++", "-std=c++23", "-O2", "-DMICROPIXEL_MODEL_TESTING",
                "-DMICROPIXEL_CANDIDATE_EVALUATOR", define,
                f"-I{WORKSPACE_ROOT / 'guest'}", f"-I{temporary_path}",
                str(WORKSPACE_ROOT / "guest/apps/tilt/tilt_model.cpp"),
                str(WORKSPACE_ROOT / "guest/apps/tilt/tilt_autoplay_test.cpp"),
                "-o", str(executable_path),
            ]
            subprocess.run(compile_command, cwd=WORKSPACE_ROOT, check=True, timeout=120)
            completed = subprocess.run([str(executable_path)], cwd=WORKSPACE_ROOT, check=True,
                                       capture_output=True, text=True, timeout=180)

            results: dict[int, list[dict[str, int]]] = {}
            for line in completed.stdout.splitlines():
                if not line.startswith("CANDIDATE "):
                    continue
                fields = line.split()
                candidate_index = int(fields[1])
                values = {key: int(value) for key, value in
                          (field.split("=", 1) for field in fields[2:])}
                results.setdefault(candidate_index, []).append(values)
            if len(results) != len(batch):
                raise RuntimeError(
                    f"physics evaluator returned {len(results)}/{len(batch)} candidate records")

            for index, candidate in enumerate(batch):
                scenarios = sorted(results[index], key=lambda result: result["scenario"])
                robust = len(scenarios) == 3 and all(
                    scenario["clear"] != 0 and scenario["falls"] == 0 for scenario in scenarios)
                times = sorted(scenario["time"] for scenario in scenarios if scenario["clear"] != 0)
                candidate["generator"]["physics"] = {
                    "robust": robust,
                    "scenario_times_ms": [scenario["time"] for scenario in scenarios],
                    "scenario_wall_hits": [scenario["walls"] for scenario in scenarios],
                    "median_time_ms": times[len(times) // 2] if times else 180000,
                    "worst_time_ms": max(times) if times else 180000,
                }


def difficulty_targets(number: int) -> tuple[int, int]:
    if number <= 10:
        ordinal = number - FIRST_GENERATED_LEVEL
        score = INTRO_TARGET_DIFFICULTY_SCORES[ordinal]
        time_ms = INTRO_TARGET_PHYSICS_TIME_MS[ordinal]
        if timing_choke_trial(number):
            score += 100
            time_ms += 5000
        return score, time_ms
    chapter, stage = campaign_position(number)
    chapter_time_ms = 50000 + chapter * 4000
    chapter_score = 1900 + chapter * 90
    choke_score = 180 if inferno_trial(number) else 100 if timing_choke_trial(number) else 0
    choke_time_ms = 10000 if inferno_trial(number) else 5000 if timing_choke_trial(number) else 0
    return (chapter_score + CHAPTER_STAGE_SCORE_OFFSETS[stage - 1] + choke_score,
            chapter_time_ms + CHAPTER_STAGE_TIME_OFFSETS_MS[stage - 1] + choke_time_ms)


def minimum_campaign_physics_time(number: int) -> int:
    chapter, stage = campaign_position(number)
    minimum_ms = 48000 + (chapter - 1) * 2500
    if stage == 5:
        minimum_ms -= 6000
    elif stage == 10:
        minimum_ms += 4000
    if inferno_trial(number):
        minimum_ms += 8000
    elif timing_choke_trial(number):
        minimum_ms += 4000
    if number == APP_TOTAL_LEVEL_COUNT:
        minimum_ms = max(minimum_ms, 110000)
    return minimum_ms


def normalize_obstacle_physics(levels: list[dict[str, object]]) -> None:
    """Keep identical obstacle art governed by identical physical constants."""
    for level in levels:
        for fan in level.get("fans", []):
            if len(fan) != 8 or fan[5] == 0:
                continue
            magnitude = math.hypot(fan[3], fan[4])
            if magnitude == 0:
                raise ValueError("pulsed fan has no force vector")
            fan[3] = round(fan[3] * PULSED_FAN_FORCE / magnitude)
            fan[4] = round(fan[4] * PULSED_FAN_FORCE / magnitude)
            fan[5] = PULSED_FAN_PERIOD_MS
            fan[6] = PULSED_FAN_ACTIVE_MS
            fan[7] %= PULSED_FAN_PERIOD_MS
        if int(level.get("bumper_period_ms", 0)) > 0:
            level["bumper_period_ms"] = MOVING_OBSTACLE_PERIOD_MS
        moving_wall = level.get("moving_wall")
        if moving_wall and moving_wall[6] > 0:
            moving_wall[6] = MOVING_OBSTACLE_PERIOD_MS
        gate = level.get("gate")
        if gate and gate[4] > 0:
            gate[4] = TIMED_GATE_PERIOD_MS
            gate[5] = TIMED_GATE_OPEN_MS
            gate[6] %= TIMED_GATE_PERIOD_MS


def select_candidate(number: int, candidates: list[dict[str, object]],
                     selected_levels: list[dict[str, object]], attempts: int) -> dict[str, object]:
    target, target_physics_time = difficulty_targets(number)
    previous = selected_levels[-1] if selected_levels else None
    previous_physics_time = 0 if previous is None else int(previous["generator"]["physics"]["median_time_ms"])
    # The proxy is useful for the six-level teaching ramp. Expert archetypes
    # are governed primarily by measured production-physics time and hazard
    # criticality; a wind-focused level should not fail merely because it has
    # fewer static route turns than a portal-focused target.
    minimum_score = target * (85 if number <= 7 else 70) // 100
    if number <= 7:
        minimum_physics_time = previous_physics_time + (0 if previous is None else
                                                        MINIMUM_PHYSICS_TIME_STEP_MS)
    else:
        # Each chapter has a higher floor, while stage five is an intentional
        # breather and stage ten is the chapter exam.
        minimum_physics_time = minimum_campaign_physics_time(number)
    used_topologies = {canonical_topology_signature(level) for level in selected_levels}
    used_mechanics = {mechanic_signature(level) for level in selected_levels}
    eligible = [candidate for candidate in candidates
                if candidate["generator"]["physics"]["robust"] and
                int(candidate["generator"]["difficulty"]["score"]) >= minimum_score and
                int(candidate["generator"]["physics"]["median_time_ms"]) >= minimum_physics_time and
                canonical_topology_signature(candidate) not in used_topologies and
                mechanic_signature(candidate) not in used_mechanics]
    if not eligible:
        robust = sum(bool(candidate["generator"]["physics"]["robust"]) for candidate in candidates)
        robust_times = [int(candidate["generator"]["physics"]["median_time_ms"])
                        for candidate in candidates if candidate["generator"]["physics"]["robust"]]
        best = max(robust_times) if robust_times else 0
        raise ValueError(
            f"level {number}: {robust}/{len(candidates)} robust candidates; best physics time {best} ms cannot "
            f"meet minimum {minimum_physics_time} ms and proxy score {minimum_score}")

    def rank(candidate: dict[str, object]) -> tuple[int, int, int, int, int, int]:
        difficulty = candidate["generator"]["difficulty"]
        score = int(difficulty["score"])
        distance = abs(score - target)
        physics_distance = abs(int(candidate["generator"]["physics"]["median_time_ms"]) - target_physics_time)
        novelty = topology_novelty(candidate, selected_levels)
        # The teaching ramp uses tight bands. Campaign chapters use wider
        # bands so topology novelty can break ties between similarly hard maps.
        physics_band_ms = 2000 if number <= 10 else 4000
        return (-physics_distance // physics_band_ms, -distance // 40, novelty,
                int(difficulty["hazard_detour_steps"]), -physics_distance - distance * 20,
                -int(candidate["generator"]["seed"]))

    selected = max(eligible, key=rank)
    difficulty = selected["generator"]["difficulty"]
    physics = selected["generator"]["physics"]
    selected["par_time_ms"] = ((int(physics["worst_time_ms"]) * 120 // 100 + 999) // 1000) * 1000
    difficulty["physics_median_time_ms"] = physics["median_time_ms"]
    difficulty["physics_worst_time_ms"] = physics["worst_time_ms"]
    selected["generator"]["selection"] = {
        "candidate_count": len(candidates),
        "attempts": attempts,
        "target_score": target,
        "minimum_score": minimum_score,
        "target_physics_time_ms": target_physics_time,
        "minimum_physics_time_ms": minimum_physics_time,
        "topology_novelty_per_mille": topology_novelty(selected, selected_levels),
        "selected_score": difficulty["score"],
    }
    return selected


def generate_catalog(generated_count: int, candidate_count: int, physics_batch_size: int,
                     selection_batch_size: int, initial_levels: list[dict[str, object]] | None = None,
                     checkpoint_path: Path | None = None) -> list[dict[str, object]]:
    selected_levels = list(initial_levels or [])
    if len(selected_levels) > generated_count:
        raise ValueError("checkpoint contains more levels than requested")
    last_number_exclusive = FIRST_GENERATED_LEVEL + generated_count
    resume_number = FIRST_GENERATED_LEVEL + len(selected_levels)
    for first_number in range(resume_number, last_number_exclusive, selection_batch_size):
        numbers = list(range(first_number, min(first_number + selection_batch_size,
                                               last_number_exclusive)))
        candidate_pools = []
        pool_metadata = []
        for number in numbers:
            level_candidate_count = (max(candidate_count, FINAL_LEVEL_CANDIDATE_COUNT)
                                     if number == APP_TOTAL_LEVEL_COUNT else candidate_count)
            candidates, attempts, rejections = generate_candidate_pool(number, level_candidate_count)
            candidate_pools.append(candidates)
            pool_metadata.append((attempts, rejections))
            print(f"level {number}: generated {len(candidates)} candidates from {attempts} attempts")
        evaluate_candidate_physics(candidate_pools, physics_batch_size)

        for number, candidates, metadata in zip(numbers, candidate_pools, pool_metadata):
            attempts, rejections = metadata
            remaining_candidates = list(candidates)
            while remaining_candidates:
                level = select_candidate(number, remaining_candidates, selected_levels, attempts)
                try:
                    validate_level_routes(level, number)
                    validate_level_challenges(level, number)
                    break
                except ValueError as error:
                    rejections[f"production validation: {error}"] += 1
                    remaining_candidates.remove(level)
            else:
                raise ValueError(f"level {number}: no physics-qualified candidate passed production validation")
            selected_levels.append(level)
            if checkpoint_path is not None:
                checkpoint = {
                    "schema_version": 1,
                    "first_level": FIRST_GENERATED_LEVEL,
                    "generated_count": len(selected_levels),
                    "levels": selected_levels,
                }
                checkpoint_path.write_text(
                    json.dumps(checkpoint, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            difficulty = level["generator"]["difficulty"]
            selection = level["generator"]["selection"]
            print(f"level {number}: seed={level['generator']['seed']} candidates={len(candidates)}/{attempts} "
                  f"score={difficulty['score']} mastery={difficulty['mastery_steps']} "
                  f"critical={difficulty['hazard_detour_steps']} "
                  f"novelty={selection['topology_novelty_per_mille']} "
                  f"physics={difficulty['physics_median_time_ms']}/{difficulty['physics_worst_time_ms']}ms "
                  f"par={level['par_time_ms']}ms")
            if rejections:
                print("  rejected: " + "; ".join(
                    f"{reason} ({count})" for reason, count in rejections.most_common(2)))
    return selected_levels


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true", help="replace levels 02-100 in levels.json")
    parser.add_argument("--output", type=Path,
                        help="write a standalone generated catalog instead of changing the app")
    parser.add_argument("--generated-count", type=int, default=DEFAULT_GENERATED_LEVEL_COUNT,
                        help="number of generated levels, beginning at level 02")
    parser.add_argument("--candidate-count", type=int, default=DEFAULT_CANDIDATE_COUNT,
                        help="number of valid candidates ranked for each level")
    parser.add_argument("--physics-batch-size", type=int, default=DEFAULT_PHYSICS_BATCH_SIZE,
                        help="maximum candidates compiled into one physics evaluator")
    parser.add_argument("--selection-batch-size", type=int, default=DEFAULT_SELECTION_BATCH_SIZE,
                        help="number of level candidate pools retained at once")
    parser.add_argument("--checkpoint", type=Path,
                        help="write selected levels after every successful level")
    parser.add_argument("--resume-from", type=Path,
                        help="resume from a checkpoint produced by this generator")
    parser.add_argument("--regenerate-from", type=int,
                        help="with --resume-from, discard this level and later before resuming")
    args = parser.parse_args()
    if min(args.generated_count, args.candidate_count, args.physics_batch_size,
           args.selection_batch_size) <= 0:
        raise SystemExit("count and batch-size arguments must be positive")
    if args.write and args.output:
        raise SystemExit("--write and --output are mutually exclusive")
    if args.write and args.generated_count != DEFAULT_GENERATED_LEVEL_COUNT:
        raise SystemExit("--write requires the app's 99 generated levels (02-100); use --output for other sizes")

    initial_levels = []
    if args.resume_from:
        resumed = json.loads(args.resume_from.read_text(encoding="utf-8"))
        if resumed.get("first_level") != FIRST_GENERATED_LEVEL:
            raise SystemExit("checkpoint starts at an incompatible level")
        initial_levels = resumed.get("levels", [])
        if args.regenerate_from is not None:
            if args.regenerate_from < FIRST_GENERATED_LEVEL:
                raise SystemExit(f"--regenerate-from must be at least {FIRST_GENERATED_LEVEL}")
            initial_levels = initial_levels[:args.regenerate_from - FIRST_GENERATED_LEVEL]
        print(f"resuming after {len(initial_levels)} generated levels")
    elif args.regenerate_from is not None:
        raise SystemExit("--regenerate-from requires --resume-from")
    generated_levels = generate_catalog(args.generated_count, args.candidate_count,
                                        args.physics_batch_size, args.selection_batch_size,
                                        initial_levels, args.checkpoint)
    if args.write:
        manifest = json.loads(LEVELS_PATH.read_text(encoding="utf-8"))
        tutorial = manifest["levels"][0]
        tutorial["bumper_end"] = [476, 280]
        tutorial["bumper_period_ms"] = MOVING_OBSTACLE_PERIOD_MS
        manifest["levels"] = [tutorial] + generated_levels
        normalize_obstacle_physics(manifest["levels"])
        LEVELS_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {LEVELS_PATH}")
    elif args.output:
        catalog = {
            "schema_version": 1,
            "first_level": FIRST_GENERATED_LEVEL,
            "generated_count": len(generated_levels),
            "levels": generated_levels,
        }
        args.output.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
