#!/usr/bin/env python3

import json
import math
import sys
import unittest
from pathlib import Path


SOURCE_DIR = Path(__file__).resolve().parents[2] / "guest/apps/tilt/assets/source"
sys.path.insert(0, str(SOURCE_DIR))

import generate_hard_levels as generator  # noqa: E402
import generate_gameplay_assets as assets  # noqa: E402


def level_with_edges(edges):
    return {
        "fans": [],
        "ice": [0, 0, 0, 0],
        "moving_wall": [0, 0, 0, 0, 0, 0, 0],
        "gate": [0, 0, 0, 0, 0, 0, 0],
        "pressure_gate": [0, 0, 0, 0, 0, 0, 0],
        "portals": [0, 0, 0, 0, 0, 0],
        "generator": {
            "topology_edges": [[list(first), list(second)] for first, second in edges],
        },
    }


class TiltLevelGeneratorTest(unittest.TestCase):
    def test_canonical_signature_rejects_rotated_and_mirrored_maze_copies(self):
        edges = [((0, 0), (0, 1)), ((0, 1), (1, 1)), ((1, 1), (1, 2))]
        original = level_with_edges(edges)
        for transform in range(8):
            transformed = level_with_edges([
                (generator.transform_cell(first, transform),
                 generator.transform_cell(second, transform))
                for first, second in edges
            ])
            self.assertEqual(generator.canonical_topology_signature(original),
                             generator.canonical_topology_signature(transformed))

    def test_campaign_targets_rise_across_ten_chapters(self):
        self.assertGreater(generator.difficulty_targets(100)[1],
                           generator.difficulty_targets(50)[1])
        self.assertGreater(generator.difficulty_targets(50)[1],
                           generator.difficulty_targets(20)[1])
        for chapter in range(2, 11):
            first = (chapter - 1) * 10 + 1
            exam = chapter * 10
            breather = first + 4
            self.assertGreater(generator.difficulty_targets(exam)[1],
                               max(generator.difficulty_targets(level)[1]
                                   for level in range(first, exam)))
            self.assertLess(generator.difficulty_targets(breather)[1],
                            generator.difficulty_targets(first + 3)[1])

    def test_late_chapters_remove_non_portal_maze_shortcuts(self):
        self.assertGreater(generator.maze_loop_count(20, False),
                           generator.maze_loop_count(100, False))
        self.assertGreaterEqual(generator.maze_loop_count(100, True), 2)
        self.assertGreater(generator.minimum_pressure_detour_steps(100),
                           generator.minimum_pressure_detour_steps(20))
        self.assertGreater(generator.minimum_portal_backtrack_steps(100),
                           generator.minimum_portal_backtrack_steps(20))
        final_profile = generator.feature_profile(100)
        self.assertTrue(final_profile["exam"])
        self.assertTrue(final_profile["second_fan"])
        self.assertFalse(final_profile["portal"])

    def test_final_chapter_has_inferno_timing_chokes(self):
        expected = {93, 96, 99, 100}
        self.assertEqual({number for number in range(1, 101) if generator.inferno_trial(number)}, expected)
        self.assertTrue(all(generator.passage_opening(number, False) == generator.INFERNO_OPENING
                            for number in expected))

        levels = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))["levels"]
        for number in expected:
            level = levels[number - 1]
            choke = level["generator"]["timing_choke"]
            self.assertEqual(choke["lateral_clearance_px"], generator.INFERNO_OPENING - 36)
            self.assertEqual(choke["tier"], "inferno")
            self.assertEqual(level["gate"][6], generator.TIMING_CHOKE_GATE_PHASE_MS)
            self.assertEqual(level["fans"][0][:2], list(generator.center(tuple(choke["fan_cell"]))))
            self.assertIn(choke["fan_cell"], choke["gate_edge"])
        self.assertGreaterEqual(generator.minimum_campaign_physics_time(100), 110000)
        self.assertGreater(
            levels[99]["generator"]["difficulty"]["physics_median_time_ms"],
            levels[98]["generator"]["difficulty"]["physics_median_time_ms"])

    def test_timing_chokes_appear_in_first_ten_levels_and_repeat(self):
        expected = {8, 10, 16, 26, 36, 46, 56, 66, 76, 86, 93, 96, 99, 100}
        self.assertEqual({number for number in range(1, 101) if generator.timing_choke_trial(number)}, expected)
        self.assertGreater(generator.passage_opening(8, False), generator.INFERNO_OPENING)
        self.assertGreater(generator.passage_opening(10, False), generator.INFERNO_OPENING)
        levels = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))["levels"]
        for number in expected:
            self.assertIn("timing_choke", levels[number - 1]["generator"])

    def test_seed_stream_supports_levels_after_legacy_chapter(self):
        first_stream = generator.candidate_seed_stream(14)
        second_stream = generator.candidate_seed_stream(14)
        self.assertEqual(next(first_stream), next(second_stream))

    def test_novelty_uses_closest_level_in_full_catalog(self):
        first = level_with_edges([((0, 0), (0, 1)), ((0, 1), (0, 2))])
        different = level_with_edges([((5, 5), (5, 4)), ((5, 4), (4, 4))])
        duplicate = level_with_edges([((0, 0), (0, 1)), ((0, 1), (0, 2))])
        self.assertGreater(generator.topology_novelty(first, [different]), 0)
        self.assertEqual(generator.topology_novelty(first, [different, duplicate]), 0)

    def test_obstacle_physics_are_normalized_independent_of_level(self):
        levels = [{
            "fans": [[0, 0, 120, 300, 400, 2200, 900, 4100]],
            "bumper_period_ms": 4200,
            "moving_wall": [0, 0, 1, 1, 2, 2, 2100],
            "gate": [0, 0, 1, 1, 2400, 700, 4100],
        }]
        generator.normalize_obstacle_physics(levels)
        fan = levels[0]["fans"][0]
        self.assertAlmostEqual(math.hypot(fan[3], fan[4]), generator.PULSED_FAN_FORCE, delta=1)
        self.assertGreaterEqual(
            generator.PULSED_FAN_FORCE * generator.PULSED_FAN_MINIMUM_SCALE -
            generator.PLAYER_MAX_TILT_ACCELERATION,
            generator.PULSED_FAN_MINIMUM_FORCE_MARGIN)
        self.assertEqual(fan[5:7], [generator.PULSED_FAN_PERIOD_MS,
                                    generator.PULSED_FAN_ACTIVE_MS])
        self.assertEqual(levels[0]["bumper_period_ms"], generator.MOVING_OBSTACLE_PERIOD_MS)
        self.assertEqual(levels[0]["moving_wall"][6], generator.MOVING_OBSTACLE_PERIOD_MS)
        self.assertEqual(levels[0]["gate"][4:6], [generator.TIMED_GATE_PERIOD_MS,
                                                   generator.TIMED_GATE_OPEN_MS])

    def test_manifest_has_100_levels_and_moving_tutorial_bumper(self):
        manifest = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))
        levels = manifest["levels"]
        self.assertEqual(len(levels), generator.APP_TOTAL_LEVEL_COUNT)
        self.assertNotEqual(levels[0]["bumper"], [*levels[0]["bumper_end"], levels[0]["bumper"][2]])
        self.assertEqual(levels[0]["bumper_period_ms"], generator.MOVING_OBSTACLE_PERIOD_MS)

    def test_every_portal_bypass_is_wide_and_outside_fan_fields(self):
        levels = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))["levels"]
        for level in levels:
            if level.get("portals", [0, 0, 0])[2] == 0:
                continue
            self.assertGreaterEqual(level["generator"]["portal_bypass_clearance_px"], 34)
            self.assertTrue(level["generator"]["portal_bypass_avoids_fans"])
            bypass = [generator.center(tuple(cell))
                      for cell in level["generator"]["portal_bypass_cells"]]
            for fan in level["fans"]:
                self.assertFalse(generator.route_hits_circle(bypass, fan[0], fan[1], fan[2]))

    def test_generated_maze_topologies_are_globally_unique(self):
        levels = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))["levels"][1:]
        signatures = [generator.canonical_topology_signature(level) for level in levels]
        self.assertEqual(len(signatures), len(set(signatures)))

    def test_no_collision_wall_is_mostly_invisible(self):
        levels = json.loads(generator.LEVELS_PATH.read_text(encoding="utf-8"))["levels"]
        for level_number, level in enumerate(levels, start=1):
            assets.validate_visual_wall_coverage(level, level_number)


if __name__ == "__main__":
    unittest.main()
