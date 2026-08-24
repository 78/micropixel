#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import copy
import json
import math
import tempfile
import unittest
from pathlib import Path


WORKSPACE_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = WORKSPACE_ROOT / "tools" / "analyze_sfx.py"
SPEC = importlib.util.spec_from_file_location("analyze_sfx", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
SFX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SFX)


FLAT_PROFILE = {
    "device": "test",
    "calibration": "flat",
    "frequency_response_db": [[20, 0.0], [8000, 0.0]],
}


def effect(waveform: str, volume: int = 100, frequency: int = 440) -> dict:
    return {
        "max_rate_hz": 1.0,
        "target_relative_db": 0.0,
        "tones": [
            {
                "waveform": waveform,
                "frequency_hz": frequency,
                "duration_ms": 120,
                "volume_per_mille": volume,
                "attack_ms": 4,
                "release_ms": 24,
                "delay_ms": 0,
            }
        ],
    }


class PerceptualAnalysisTest(unittest.TestCase):
    def metrics(self, profile: dict, waveform: str = "sine", volume: int = 100, frequency: int = 440) -> dict:
        samples = SFX.synthesize_effect(effect(waveform, volume, frequency), 16000)
        return SFX.analyze_samples(samples, 16000, profile, 1.0)

    def test_double_amplitude_is_six_db(self) -> None:
        quiet = self.metrics(FLAT_PROFILE, volume=100)
        loud = self.metrics(FLAT_PROFILE, volume=200)
        difference = loud["a_weighted_event_dbfs_s"] - quiet["a_weighted_event_dbfs_s"]
        self.assertAlmostEqual(difference, 20.0 * math.log10(2.0), delta=0.15)

    def test_momentary_level_ignores_trailing_silence(self) -> None:
        samples = SFX.synthesize_effect(effect("sine"), 16000)
        original = SFX._maximum_window_rms_dbfs(samples, 16000)
        padded = SFX._maximum_window_rms_dbfs(samples + [0.0] * 16000, 16000)
        self.assertAlmostEqual(padded, original, delta=0.01)

    def test_relative_transient_does_not_penalize_amplitude(self) -> None:
        quiet = self.metrics(FLAT_PROFILE, volume=100)
        loud = self.metrics(FLAT_PROFILE, volume=400)
        self.assertAlmostEqual(quiet["transient_delta_relative_db"], loud["transient_delta_relative_db"], delta=0.1)

    def test_square_is_sharper_than_triangle(self) -> None:
        square = self.metrics(FLAT_PROFILE, waveform="square")
        triangle = self.metrics(FLAT_PROFILE, waveform="triangle")
        self.assertGreater(square["high_frequency_ratio"], triangle["high_frequency_ratio"] * 8.0)
        self.assertGreater(square["spectral_centroid_hz"], triangle["spectral_centroid_hz"])

    def test_repetition_rate_uses_energy_sum(self) -> None:
        samples = SFX.synthesize_effect(effect("triangle"), 16000)
        once = SFX.analyze_samples(samples, 16000, FLAT_PROFILE, 1.0)
        four = SFX.analyze_samples(samples, 16000, FLAT_PROFILE, 4.0)
        self.assertAlmostEqual(four["repetition_exposure_dbfs"] - once["repetition_exposure_dbfs"],
                               10.0 * math.log10(4.0), delta=0.01)

    def test_device_response_changes_predicted_level(self) -> None:
        attenuated = {
            "device": "test",
            "calibration": "measured",
            "frequency_response_db": [[20, -20.0], [250, -20.0], [500, 0.0], [8000, 0.0]],
        }
        flat = self.metrics(FLAT_PROFILE, frequency=120)
        filtered = self.metrics(attenuated, frequency=120)
        self.assertLess(filtered["a_weighted_event_dbfs_s"], flat["a_weighted_event_dbfs_s"] - 15.0)

    def test_manifest_generates_runtime_header(self) -> None:
        manifest = SFX.load_manifest(WORKSPACE_ROOT / "guest" / "apps" / "blocks" / "audio" / "sfx.json")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "profiles.hpp"
            SFX.emit_cpp_header(manifest, output)
            generated = output.read_text(encoding="utf-8")
        self.assertIn("kHardDrop", generated)
        self.assertIn("kLevelUp", generated)
        self.assertIn("kMoveCount", generated)
        self.assertNotIn("kMasterPercent", generated)

    def test_snake_manifest_generates_runtime_header(self) -> None:
        manifest = SFX.load_manifest(WORKSPACE_ROOT / "guest" / "apps" / "snake" / "audio" / "sfx.json")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "profiles.hpp"
            SFX.emit_cpp_header(manifest, output)
            generated = output.read_text(encoding="utf-8")
        self.assertIn("kFoodNormal", generated)
        self.assertIn("kFoodPoison", generated)
        self.assertIn("kBgmBCount", generated)
        self.assertNotIn("kMasterPercent", generated)

    def test_guest_master_attenuation_is_rejected(self) -> None:
        source = WORKSPACE_ROOT / "guest" / "apps" / "blocks" / "audio" / "sfx.json"
        manifest = json.loads(source.read_text(encoding="utf-8"))
        manifest["master_percent"] = 45
        with tempfile.TemporaryDirectory() as directory:
            legacy = Path(directory) / "legacy-sfx.json"
            legacy.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "master_percent is obsolete"):
                SFX.load_manifest(legacy)

    def test_audio_games_have_valid_checked_manifests(self) -> None:
        device_profile = SFX.load_device_profile(
            WORKSPACE_ROOT / "firmware" / "espressif" / "main" / "platform" / "metalio-claw4" / "audio"
            / "perceptual_profile.json"
        )
        checked_games = []
        for app_manifest_path in sorted((WORKSPACE_ROOT / "guest" / "apps").glob("*/app.json")):
            app_manifest = json.loads(app_manifest_path.read_text(encoding="utf-8"))
            sources = app_manifest.get("sources", [])
            if not any(Path(source).parent == Path(".") and Path(source).name.endswith("_audio.cpp")
                       for source in sources):
                continue
            game_directory = app_manifest_path.parent
            sfx_manifest_path = game_directory / "audio" / "sfx.json"
            self.assertTrue(sfx_manifest_path.is_file(), f"{game_directory.name} audio requires audio/sfx.json")
            report = SFX.analyze_manifest(SFX.load_manifest(sfx_manifest_path), device_profile)
            self.assertFalse(report["violations"], f"{game_directory.name} audio violations: {report['violations']}")
            checked_games.append(game_directory.name)
        self.assertIn("blocks", checked_games)
        self.assertIn("snake", checked_games)

    def test_uniformly_quiet_manifest_fails_absolute_level_gate(self) -> None:
        source = WORKSPACE_ROOT / "guest" / "apps" / "blocks" / "audio" / "sfx.json"
        manifest = SFX.load_manifest(source)
        quiet = copy.deepcopy(manifest)
        for effect_data in quiet["effects"].values():
            for tone in effect_data["tones"]:
                tone["volume_per_mille"] = max(1, tone["volume_per_mille"] // 4)
        report = SFX.analyze_manifest(quiet, FLAT_PROFILE)
        self.assertTrue(any("momentary level" in violation and "too quiet" in violation
                            for violation in report["violations"]))

    def test_game_audio_template_passes_default_constraints(self) -> None:
        manifest = SFX.load_manifest(WORKSPACE_ROOT / "docs" / "development" / "game-sfx.template.json")
        report = SFX.analyze_manifest(manifest, FLAT_PROFILE)
        self.assertFalse(report["violations"])

    def test_harsh_move_regression_fails_constraints(self) -> None:
        manifest = SFX.load_manifest(WORKSPACE_ROOT / "guest" / "apps" / "blocks" / "audio" / "sfx.json")
        changed = copy.deepcopy(manifest)
        changed["effects"]["move"]["tones"][0]["waveform"] = "square"
        report = SFX.analyze_manifest(changed, FLAT_PROFILE)
        self.assertTrue(any(violation.startswith("move:") for violation in report["violations"]))


if __name__ == "__main__":
    unittest.main()
