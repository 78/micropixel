#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author the App's Ogg Opus assets from the recordings section of sfx.json.

PCM intermediates and measurements stay in build/. The resulting compressed
tracks are source assets, consumed by the ordinary Bundle resource pipeline.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import random
from pathlib import Path
import struct
import subprocess
import wave

ROOT = Path(__file__).resolve().parents[4]
SPEC = importlib.util.spec_from_file_location("jump_jump_sfx_analyzer", ROOT / "tools/analyze_sfx.py")
SFX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SFX)
APP = Path(__file__).resolve().parents[1]


def synthesize(track: dict, sample_rate: int) -> list[float]:
    kind = track["kind"]
    gain = track["volume_per_mille"] / 1000.0
    if kind == "water":
        count = track["duration_ms"] * sample_rate // 1000
        rng = random.Random(track["seed"])
        output = []
        low = 0.0
        smooth = 0.0
        phase = 0.0
        alpha = 1.0 - math.exp(-2 * math.pi * track["cutoff_hz"] / sample_rate)
        for i in range(count):
            time = i / sample_rate
            t = i / count
            low += alpha * (rng.uniform(-1, 1) - low)
            smooth += alpha * (low - smooth)
            envelope = min(1.0, time * 1000 / track["attack_ms"],
                           (count - 1 - i) * 1000 / sample_rate / track["release_ms"])
            envelope *= (1.0 - t) ** track["decay_power"]
            wobble = 0.5 + 0.5 * math.sin(2 * math.pi * track["bubble_rate_hz"] * time)
            frequency = track["bubble_start_hz"] * (track["bubble_end_hz"] / track["bubble_start_hz"]) ** t
            phase += 2 * math.pi * frequency / sample_rate
            output.append(gain * envelope * (smooth + track["bubble_gain"] * wobble * math.sin(phase)))
        return output
    if kind != "music_box":
        raise ValueError(f"Unknown recording kind: {kind}")
    beat = 60.0 / track["bpm"]
    length = sum(note[1] for note in track["melody"]) * beat + track["tail_ms"] / 1000
    output = [0.0] * math.ceil(length * sample_rate)

    def note(at: float, frequency: float, duration: float, amplitude: float, decay: float) -> None:
        start = round(at * sample_rate)
        count = min(round(duration * sample_rate), len(output) - start)
        for i in range(count):
            time = i / sample_rate
            attack = min(1.0, time * 1000 / track["attack_ms"])
            release = min(1.0, (count - 1 - i) * 1000 / sample_rate / track["release_ms"])
            envelope = math.exp(-decay * time) * attack * release
            value = sum(level * math.sin(2 * math.pi * frequency * (h + 1) * time)
                        for h, level in enumerate(track["harmonics"]))
            output[start + i] += amplitude * envelope * value

    at = 0.0
    for frequency, beats in track["melody"]:
        note(at, frequency, beats * beat + track["note_tail_ms"] / 1000, gain, track["melody_decay"])
        at += beats * beat
    for bar, chord in enumerate(track["chords"]):
        for index, frequency in enumerate(chord):
            note(bar * track.get("beats_per_bar", 4) * beat + index * track["arpeggio_beats"] * beat, frequency,
                 track["chord_duration_beats"] * beat, gain * track["chord_gain"], track["chord_decay"])
    return output


def analyze(manifest: dict) -> tuple[dict, dict[str, list[float]]]:
    rendered = {}
    report = {"effects": {}, "violations": []}
    for name, track in manifest["recordings"].items():
        samples = synthesize(track, manifest["sample_rate_hz"])
        rendered[name] = samples
        if any(abs(sample) >= 1.0 for sample in samples):
            report["violations"].append(f"{name}: PCM clipping")
        metrics = SFX.analyze_samples(samples, manifest["sample_rate_hz"], track["max_rate_hz"])
        target = manifest["reference_momentary_rms_dbfs"] + track["target_relative_db"]
        metrics["momentary_target_dbfs"] = target
        metrics["recommended_volume_scale"] = 10 ** ((target - metrics["momentary_rms_dbfs"]) / 20)
        report["effects"][name] = metrics
        for metric, limit in (("peak_dbfs", "peak_dbfs_max"), ("high_frequency_ratio", "high_frequency_ratio_max"),
                              ("transient_delta_relative_db", "transient_delta_relative_db_max")):
            if metrics[metric] > manifest["limits"][limit]:
                report["violations"].append(f"{name}: {metric} exceeds {limit}")
        if abs(metrics["momentary_rms_dbfs"] - target) > manifest["limits"]["momentary_tolerance_db"]:
            report["violations"].append(f"{name}: momentary level differs from target")
    return report, rendered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Analyze only; do not regenerate compressed source assets")
    args = parser.parse_args()
    manifest = SFX.load_manifest(APP / "audio/sfx.json")
    report, rendered = analyze(manifest)
    scratch = ROOT / "build/apps/jump-jump/audio"
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / "recordings-report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, metrics in report["effects"].items():
        print(f"{name}: short={metrics['momentary_rms_dbfs']:.1f} dBFS "
              f"target={metrics['momentary_target_dbfs']:.1f} peak={metrics['peak_dbfs']:.1f} "
              f"gain hint={metrics['recommended_volume_scale']:.3f}")
    if report["violations"]:
        print("\n".join(report["violations"]))
        return 1
    if args.check:
        return 0
    assets = APP / "assets"
    assets.mkdir(exist_ok=True)
    for name, samples in rendered.items():
        wav = scratch / f"{name}.wav"
        with wave.open(str(wav), "wb") as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(manifest["sample_rate_hz"])
            out.writeframes(b"".join(struct.pack("<h", round(sample * 32767)) for sample in samples))
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-c:a", "libopus",
                        "-application", "audio", "-ac", "1", "-b:a", "40k", "-vbr", "on",
                        str(assets / f"{name}.ogg")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
