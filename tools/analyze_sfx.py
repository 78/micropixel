#!/usr/bin/env python3
"""Analyze and generate deterministic MicroPixel tone profiles.

The synthesizer mirrors the ESP32-P4 Host's 16 kHz integer tone mixer. The
metrics are engineering proxies for tuning and regression detection, not a
medical or standards-certified hearing-safety measurement. Absolute SPL still
requires a calibrated microphone measurement of the finished device.
"""

from __future__ import annotations

import argparse
import cmath
import json
import math
import re
import struct
import sys
import wave
from pathlib import Path
from typing import Any


WAVEFORMS = {"sine", "square", "triangle", "noise"}
CPP_WAVEFORMS = {
    "sine": "micropixel::Waveform::kSine",
    "square": "micropixel::Waveform::kSquare",
    "triangle": "micropixel::Waveform::kTriangle",
    "noise": "micropixel::Waveform::kNoise",
}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def load_manifest(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    _require(data.get("schema_version") == 1, "SFX manifest schema_version must be 1")
    sample_rate = data.get("sample_rate_hz")
    _require(isinstance(sample_rate, int) and 8000 <= sample_rate <= 48000, "invalid sample_rate_hz")
    _require("master_percent" not in data, "master_percent is obsolete; Host owns the device master volume")
    cpp_namespace = data.get("cpp_namespace", "sfx_profiles")
    header_guard = data.get("header_guard", "MICROPIXEL_SFX_PROFILES_HPP")
    _require(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", cpp_namespace) is not None, "invalid cpp_namespace")
    _require(re.fullmatch(r"[A-Z_][A-Z0-9_]*", header_guard) is not None, "invalid header_guard")
    effects = data.get("effects")
    _require(isinstance(effects, dict) and effects, "effects must be a non-empty object")
    reference = data.get("reference_effect")
    _require(reference in effects, "reference_effect must name an effect")
    for effect_name, effect in effects.items():
        _require(re.fullmatch(r"[a-z][a-z0-9_]*", effect_name) is not None, f"invalid effect name: {effect_name}")
        _require(isinstance(effect, dict), f"effect {effect_name} must be an object")
        _require(isinstance(effect.get("max_rate_hz"), (int, float)) and effect["max_rate_hz"] > 0,
                 f"effect {effect_name} requires max_rate_hz > 0")
        _require(isinstance(effect.get("target_relative_db"), (int, float)),
                 f"effect {effect_name} requires target_relative_db")
        tones = effect.get("tones")
        _require(isinstance(tones, list) and tones, f"effect {effect_name} requires tones")
        _require(len(tones) <= 8, f"effect {effect_name} exceeds the Host's 8 synth voices")
        for index, tone in enumerate(tones):
            prefix = f"effect {effect_name} tone {index}"
            _require(isinstance(tone, dict), f"{prefix} must be an object")
            _require(tone.get("waveform") in WAVEFORMS, f"{prefix} has invalid waveform")
            frequency = tone.get("frequency_hz")
            if tone["waveform"] == "noise":
                _require(frequency == 0, f"{prefix} noise frequency must be 0")
            else:
                _require(isinstance(frequency, int) and 20 <= frequency <= 20000, f"{prefix} frequency invalid")
            for field, minimum, maximum in (
                ("duration_ms", 1, 2000),
                ("volume_per_mille", 0, 1000),
                ("attack_ms", 0, 2000),
                ("release_ms", 0, 2000),
                ("delay_ms", 0, 10000),
            ):
                value = tone.get(field)
                _require(isinstance(value, int) and minimum <= value <= maximum, f"{prefix} {field} invalid")
            _require(tone["attack_ms"] <= tone["duration_ms"], f"{prefix} attack exceeds duration")
            _require(tone["release_ms"] <= tone["duration_ms"], f"{prefix} release exceeds duration")
    limits = data.get("limits")
    _require(isinstance(limits, dict), "limits must be an object")
    for field in (
        "peak_dbfs_max",
        "step_exposure_dbfs_max",
        "high_frequency_ratio_max",
        "transient_delta_dbfs_max",
        "relative_tolerance_db",
    ):
        _require(isinstance(limits.get(field), (int, float)), f"limit {field} missing")
    return data


def load_device_profile(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {
            "device": "flat digital reference",
            "calibration": "uncalibrated-flat",
            "frequency_response_db": [[20, 0.0], [8000, 0.0]],
        }
    data = json.loads(path.read_text(encoding="utf-8"))
    _require(data.get("schema_version") == 1, "device profile schema_version must be 1")
    points = data.get("frequency_response_db")
    _require(isinstance(points, list) and len(points) >= 2, "device profile needs at least two response points")
    previous = 0.0
    for point in points:
        _require(isinstance(point, list) and len(point) == 2, "frequency response points must be [Hz, dB]")
        frequency, gain = point
        _require(isinstance(frequency, (int, float)) and frequency > previous, "response frequencies must increase")
        _require(isinstance(gain, (int, float)), "response gain must be numeric")
        previous = float(frequency)
    return data


def _wave_sample(waveform: str, phase: int, noise: int, sine_table: list[int]) -> tuple[int, int]:
    if waveform == "sine":
        return sine_table[(phase >> 24) & 0xFF], noise
    if waveform == "square":
        return (32767 if phase & 0x80000000 == 0 else -32768), noise
    if waveform == "triangle":
        position = phase >> 16
        sample = position * 2 - 32768 if position < 32768 else 98303 - position * 2
        return sample, noise
    noise = (noise * 1664525 + 1013904223) & 0xFFFFFFFF
    sample = (noise >> 16) & 0xFFFF
    if sample >= 0x8000:
        sample -= 0x10000
    return sample, noise


def synthesize_effect(effect: dict[str, Any], sample_rate: int) -> list[float]:
    total_ms = max(tone["delay_ms"] + tone["duration_ms"] for tone in effect["tones"])
    frame_count = max(1, total_ms * sample_rate // 1000)
    voices: list[list[int]] = [[0] * frame_count for _ in effect["tones"]]
    sine_table = [int(math.sin(2.0 * math.pi * index / 256.0) * 32767.0) for index in range(256)]
    for voice, tone in zip(voices, effect["tones"]):
        start = tone["delay_ms"] * sample_rate // 1000
        duration = tone["duration_ms"] * sample_rate // 1000
        attack = tone["attack_ms"] * sample_rate // 1000
        release = tone["release_ms"] * sample_rate // 1000
        volume = tone["volume_per_mille"]
        phase = 0
        phase_step = tone["frequency_hz"] * (1 << 32) // sample_rate if tone["waveform"] != "noise" else 0
        noise = 0x51A9E21D ^ (tone["frequency_hz"] * 1000) ^ tone["duration_ms"]
        for played in range(duration):
            remaining = duration - played
            gain = volume
            if attack and played < attack:
                gain = gain * played // attack
            if release and remaining < release:
                release_gain = volume * remaining // release
                gain = min(gain, release_gain)
            wave_sample, noise = _wave_sample(tone["waveform"], phase, noise, sine_table)
            voice[start + played] = int(wave_sample * gain / 1000)
            phase = (phase + phase_step) & 0xFFFFFFFF
    result: list[float] = []
    for frame in range(frame_count):
        mixed = sum(voice[frame] for voice in voices)
        mixed = min(32767, max(-32768, mixed))
        result.append(mixed / 32768.0)
    return result


def _fft(values: list[complex]) -> list[complex]:
    size = len(values)
    _require(size > 0 and size & (size - 1) == 0, "FFT input must have power-of-two size")
    output = list(values)
    target = 0
    for source in range(1, size):
        bit = size >> 1
        while target & bit:
            target ^= bit
            bit >>= 1
        target ^= bit
        if source < target:
            output[source], output[target] = output[target], output[source]
    length = 2
    while length <= size:
        root = cmath.exp(-2j * math.pi / length)
        half = length // 2
        for offset in range(0, size, length):
            factor = 1.0 + 0.0j
            for index in range(offset, offset + half):
                even = output[index]
                odd = output[index + half] * factor
                output[index] = even + odd
                output[index + half] = even - odd
                factor *= root
        length *= 2
    return output


def _a_weighting_db(frequency: float) -> float:
    if frequency <= 0.0:
        return -200.0
    squared = frequency * frequency
    numerator = (12200.0**2) * (squared**2)
    denominator = (squared + 20.6**2) * math.sqrt((squared + 107.7**2) * (squared + 737.9**2))
    denominator *= squared + 12200.0**2
    return 20.0 * math.log10(numerator / denominator) + 2.0


def _response_gain_db(profile: dict[str, Any], frequency: float) -> float:
    points = profile["frequency_response_db"]
    if frequency <= points[0][0]:
        return float(points[0][1])
    if frequency >= points[-1][0]:
        return float(points[-1][1])
    log_frequency = math.log(frequency)
    for left, right in zip(points, points[1:]):
        if frequency <= right[0]:
            span = math.log(right[0]) - math.log(left[0])
            position = (log_frequency - math.log(left[0])) / span
            return float(left[1]) + (float(right[1]) - float(left[1])) * position
    raise AssertionError("unreachable response interpolation")


def _db(value: float, power: bool = False) -> float:
    if value <= 1.0e-20:
        return -200.0
    return (10.0 if power else 20.0) * math.log10(value)


def analyze_samples(samples: list[float], sample_rate: int, profile: dict[str, Any], max_rate_hz: float) -> dict[str, float]:
    fft_size = 1
    while fft_size < len(samples):
        fft_size <<= 1
    spectrum = _fft([complex(sample, 0.0) for sample in samples] + [0j] * (fft_size - len(samples)))
    weighted_energy_spectrum = 0.0
    response_energy_spectrum = 0.0
    high_energy_spectrum = 0.0
    centroid_numerator = 0.0
    for index in range(fft_size // 2 + 1):
        frequency = index * sample_rate / fft_size
        multiplicity = 1.0 if index == 0 or index == fft_size // 2 else 2.0
        power = abs(spectrum[index]) ** 2 * multiplicity
        response_gain = 10.0 ** (_response_gain_db(profile, max(20.0, frequency)) / 10.0)
        adjusted_power = power * response_gain
        response_energy_spectrum += adjusted_power
        centroid_numerator += frequency * adjusted_power
        if frequency >= 2000.0:
            high_energy_spectrum += adjusted_power
        a_gain = 10.0 ** (_a_weighting_db(frequency) / 10.0)
        weighted_energy_spectrum += adjusted_power * a_gain
    weighted_energy = weighted_energy_spectrum / fft_size
    event_level = _db(weighted_energy / sample_rate, power=True)
    repetition_level = event_level + 10.0 * math.log10(max_rate_hz)
    peak = max(abs(sample) for sample in samples)
    transient = max(abs(samples[index] - samples[index - 1]) for index in range(1, len(samples)))
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
    centroid = centroid_numerator / response_energy_spectrum if response_energy_spectrum else 0.0
    high_ratio = high_energy_spectrum / response_energy_spectrum if response_energy_spectrum else 0.0
    return {
        "duration_ms": len(samples) * 1000.0 / sample_rate,
        "rms_dbfs": _db(rms),
        "peak_dbfs": _db(peak),
        "transient_delta_dbfs": _db(transient),
        "a_weighted_event_dbfs_s": event_level,
        "repetition_exposure_dbfs": repetition_level,
        "spectral_centroid_hz": centroid,
        "high_frequency_ratio": high_ratio,
    }


def analyze_manifest(manifest: dict[str, Any], profile: dict[str, Any]) -> dict[str, Any]:
    sample_rate = manifest["sample_rate_hz"]
    analyses: dict[str, dict[str, Any]] = {}
    rendered: dict[str, list[float]] = {}
    for name, effect in manifest["effects"].items():
        samples = synthesize_effect(effect, sample_rate)
        rendered[name] = samples
        metrics = analyze_samples(samples, sample_rate, profile, float(effect["max_rate_hz"]))
        analyses[name] = {**metrics, "target_relative_db": float(effect["target_relative_db"])}
    reference_level = analyses[manifest["reference_effect"]]["a_weighted_event_dbfs_s"]
    limits = manifest["limits"]
    violations: list[str] = []
    for name, metrics in analyses.items():
        relative = metrics["a_weighted_event_dbfs_s"] - reference_level
        relative_error = relative - metrics["target_relative_db"]
        metrics["relative_to_reference_db"] = relative
        metrics["relative_error_db"] = relative_error
        metrics["recommended_volume_scale"] = 10.0 ** (-relative_error / 20.0)
        penalties = abs(relative_error) * 1.5 + metrics["high_frequency_ratio"] * 30.0
        if metrics["peak_dbfs"] > limits["peak_dbfs_max"]:
            excess = metrics["peak_dbfs"] - limits["peak_dbfs_max"]
            penalties += excess * 4.0
            violations.append(f"{name}: peak exceeds limit by {excess:.1f} dB")
        if metrics["high_frequency_ratio"] > limits["high_frequency_ratio_max"]:
            excess = metrics["high_frequency_ratio"] - limits["high_frequency_ratio_max"]
            penalties += excess * 100.0
            violations.append(f"{name}: high-frequency ratio exceeds limit by {excess:.3f}")
        if metrics["transient_delta_dbfs"] > limits["transient_delta_dbfs_max"]:
            excess = metrics["transient_delta_dbfs"] - limits["transient_delta_dbfs_max"]
            penalties += excess * 3.0
            violations.append(f"{name}: transient exceeds limit by {excess:.1f} dB")
        if manifest["effects"][name].get("check_repetition_exposure", False) and metrics[
            "repetition_exposure_dbfs"
        ] > limits["step_exposure_dbfs_max"]:
            excess = metrics["repetition_exposure_dbfs"] - limits["step_exposure_dbfs_max"]
            penalties += excess * 4.0
            violations.append(f"{name}: repetition exposure exceeds limit by {excess:.1f} dB")
        tolerance = limits["relative_tolerance_db"]
        if abs(relative_error) > tolerance:
            excess = abs(relative_error) - tolerance
            penalties += excess * 2.0
            violations.append(f"{name}: hierarchy target missed by {abs(relative_error):.1f} dB")
        metrics["comfort_score"] = max(0.0, 100.0 - penalties)
    return {
        "schema_version": 1,
        "sample_rate_hz": sample_rate,
        "reference_effect": manifest["reference_effect"],
        "device": profile.get("device", "unknown"),
        "calibration": profile.get("calibration", "unknown"),
        "effects": analyses,
        "violations": violations,
        "_rendered": rendered,
    }


def _cpp_identifier(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def emit_cpp_header(manifest: dict[str, Any], path: Path) -> None:
    header_guard = manifest.get("header_guard", "MICROPIXEL_SFX_PROFILES_HPP")
    cpp_namespace = manifest.get("cpp_namespace", "sfx_profiles")
    lines = [
        f"#ifndef {header_guard}",
        f"#define {header_guard}",
        "",
        "#include <stdint.h>",
        "",
        '#include "sdk/audio.hpp"',
        "",
        f"namespace {cpp_namespace} {{",
        "",
        "struct ToneSpec final {",
        "    micropixel::Waveform waveform{};",
        "    uint32_t frequency_hz{};",
        "    uint16_t duration_ms{};",
        "    uint16_t volume_per_mille{};",
        "    uint16_t attack_ms{};",
        "    uint16_t release_ms{};",
        "    uint16_t delay_ms{};",
        "};",
        "",
    ]
    for name, effect in manifest["effects"].items():
        identifier = _cpp_identifier(name)
        lines.append(f"inline constexpr ToneSpec k{identifier}[] = {{")
        for tone in effect["tones"]:
            lines.append(
                "    {"
                f"{CPP_WAVEFORMS[tone['waveform']]}, {tone['frequency_hz']}U, {tone['duration_ms']}U, "
                f"{tone['volume_per_mille']}U, {tone['attack_ms']}U, {tone['release_ms']}U, {tone['delay_ms']}U"
                "},"
            )
        lines.append("};")
        lines.append(f"inline constexpr uint32_t k{identifier}Count = sizeof(k{identifier}) / sizeof(k{identifier}[0]);")
        lines.append("")
    lines.extend([f"}}  // namespace {cpp_namespace}", "", "#endif", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_wavs(report: dict[str, Any], directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, samples in report["_rendered"].items():
        with wave.open(str(directory / f"{name}.wav"), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(report["sample_rate_hz"])
            frames = b"".join(struct.pack("<h", min(32767, max(-32768, round(sample * 32768.0)))) for sample in samples)
            output.writeframes(frames)


def printable_report(report: dict[str, Any]) -> str:
    lines = [
        f"SFX perceptual report: {report['device']} ({report['calibration']}), "
        f"{report['sample_rate_hz']} Hz, Guest gain=unity",
        "effect       score  event A   relative/target  repeat A  peak    HF ratio  transient  gain hint",
    ]
    for name, metrics in report["effects"].items():
        lines.append(
            f"{name:11} {metrics['comfort_score']:5.1f}  {metrics['a_weighted_event_dbfs_s']:7.1f}  "
            f"{metrics['relative_to_reference_db']:7.1f}/{metrics['target_relative_db']:5.1f}  "
            f"{metrics['repetition_exposure_dbfs']:7.1f}  {metrics['peak_dbfs']:6.1f}  "
            f"{metrics['high_frequency_ratio']:8.3f}  {metrics['transient_delta_dbfs']:9.1f}  "
            f"x{metrics['recommended_volume_scale']:.2f}"
        )
    if report["violations"]:
        lines.append("violations:")
        lines.extend(f"  - {violation}" for violation in report["violations"])
    else:
        lines.append("perceptual constraints passed")
    return "\n".join(lines)


def serializable_report(report: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in report.items() if key != "_rendered"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--device-profile", type=Path)
    parser.add_argument("--emit-cpp-header", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--write-wavs", type=Path)
    parser.add_argument("--check", action="store_true", help="fail when a perceptual constraint is violated")
    args = parser.parse_args()
    try:
        manifest = load_manifest(args.manifest)
        profile = load_device_profile(args.device_profile)
        report = analyze_manifest(manifest, profile)
        if args.emit_cpp_header:
            emit_cpp_header(manifest, args.emit_cpp_header)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(serializable_report(report), indent=2) + "\n", encoding="utf-8")
        if args.write_wavs:
            write_wavs(report, args.write_wavs)
        print(printable_report(report))
        return 1 if args.check and report["violations"] else 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"SFX analysis failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
