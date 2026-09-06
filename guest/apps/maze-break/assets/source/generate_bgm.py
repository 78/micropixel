#!/usr/bin/env python3
"""Render the Maze Break background loop and encode it as Ogg Opus.

The loop reproduces the native demo's synth music: a palm-muted E minor bass
riff in eighth notes at 100 BPM, a kick on beats 1 and 3 with hat ticks on the
off-beats, and a slowly gliding three-voice sine pad over Em / C / D / Bm. Four
bars (19.2 s) are rendered at 48 kHz mono and encoded with ffmpeg's libopus so
the Host decoder plays it back at 16 kHz.

Usage:
    python3 generate_bgm.py [--output ../bgm_loop.ogg] [--bitrate 32k]

Requires ffmpeg with libopus on PATH. Pure Python is used for synthesis so no
third-party module is needed.
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SAMPLE_RATE = 48000
BPM = 100.0
STEP_SECONDS = 60.0 / BPM / 2.0  # eighth notes
STEPS_PER_BAR = 16
BARS = 4
MUSIC_GAIN = 0.20

E2, G2, B2, C3, D3, E3 = 82.41, 98.0, 123.47, 130.81, 146.83, 164.81
RIFF = [E2, E2, E3, E2, E2, D3, E2, C3, E2, E2, E3, E2, E2, B2, E2, G2]
CHORDS = [
    (164.81, 196.0, 246.94),  # Em
    (130.81, 164.81, 196.0),  # C
    (146.83, 185.0, 220.0),  # D
    (123.47, 146.83, 185.0),  # Bm
]


def fast_sin(phase: float) -> float:
    """Two parabolic humps, the same cheap sine the native synth uses."""
    return 16.0 * phase * (0.5 - phase) if phase < 0.5 else -16.0 * (phase - 0.5) * (1.0 - phase)


def wrap(phase: float) -> float:
    return phase - math.floor(phase)


def render() -> list[float]:
    dt = 1.0 / SAMPLE_RATE
    total_steps = STEPS_PER_BAR * BARS
    frames = int(round(total_steps * STEP_SECONDS * SAMPLE_RATE))
    out = [0.0] * frames

    step = -1
    step_time = 0.0
    bass_phase = sub_phase = lfo_phase = 0.0
    pad_phase = [0.0, 0.0, 0.0]
    pad_freq = [0.0, 0.0, 0.0]
    pad_target = [0.0, 0.0, 0.0]
    pad_gain = 0.0
    noise = 0x2545F491

    for i in range(frames):
        step_time += dt
        if step < 0 or step_time >= STEP_SECONDS:
            step_time = 0.0 if step < 0 else step_time - STEP_SECONDS
            step = (step + 1) % total_steps
            bar = step // STEPS_PER_BAR
            for k in range(3):
                pad_target[k] = CHORDS[bar][k]
                if pad_freq[k] == 0.0:
                    pad_freq[k] = pad_target[k]
        in_step = step_time
        local = step % STEPS_PER_BAR

        bass = 0.0
        bass_freq = RIFF[local]
        if bass_freq > 0.0:
            env = (1.0 - in_step / 0.19) if in_step < 0.19 else 0.0
            bass_phase = wrap(bass_phase + bass_freq * dt)
            sub_phase = wrap(sub_phase + bass_freq * 0.5 * dt)
            square = 1.0 if bass_phase < 0.5 else -1.0
            bass = (square * 0.55 + fast_sin(sub_phase) * 0.9) * env * env

        drums = 0.0
        if (local & 3) == 0 and in_step < 0.14:
            env = 1.0 - in_step / 0.14
            f = 45.0 + 90.0 * env * env
            lfo_phase = wrap(lfo_phase + f * dt)
            drums += fast_sin(lfo_phase) * env * 1.1
        elif (local & 1) == 1 and in_step < 0.03:
            noise ^= (noise << 13) & 0xFFFFFFFF
            noise ^= noise >> 17
            noise ^= (noise << 5) & 0xFFFFFFFF
            signed = noise - 0x100000000 if noise >= 0x80000000 else noise
            drums += signed / 2147483648.0 * (1.0 - in_step / 0.03) * 0.18

        pad = 0.0
        for k in range(3):
            # The native synth glides at 16 kHz; scale the coefficient so the
            # glide time is identical at 48 kHz.
            pad_freq[k] += (pad_target[k] - pad_freq[k]) * (0.00004 * 16000.0 / SAMPLE_RATE)
            pad_phase[k] = wrap(pad_phase[k] + pad_freq[k] * dt)
            pad += fast_sin(pad_phase[k])
        pad_gain = wrap(pad_gain + 0.35 * dt)
        pad *= 0.16 * (0.75 + 0.25 * fast_sin(pad_gain))

        out[i] = (bass + drums + pad) * MUSIC_GAIN
    return out


def write_wav(path: Path, samples: list[float]) -> None:
    peak = max(abs(s) for s in samples) or 1.0
    scale = min(1.0, 0.89 / peak)  # leave 1 dB of headroom
    data = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767.0)) for s in samples)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + len(data),
        b"WAVE",
        b"fmt ",
        16,
        1,
        1,
        SAMPLE_RATE,
        SAMPLE_RATE * 2,
        2,
        16,
        b"data",
        len(data),
    )
    path.write_bytes(header + data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "bgm_loop.ogg")
    parser.add_argument("--bitrate", default="32k")
    args = parser.parse_args()
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        print("ffmpeg not found on PATH", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as directory:
        wav = Path(directory) / "bgm.wav"
        write_wav(wav, render())
        subprocess.run(
            [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(wav),
                "-c:a",
                "libopus",
                "-b:a",
                args.bitrate,
                "-vbr",
                "constrained",
                "-application",
                "audio",
                "-frame_duration",
                "20",
                str(args.output),
            ],
            check=True,
        )
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
