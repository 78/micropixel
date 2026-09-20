# Jump Jump

A one-touch, fixed-camera platform hopping game with adaptive display sizing. Hold anywhere to squash the character and charge a
jump; release to leap towards the next platform. Confirm/South also supports
press and release. After falling, tap to restart. The first press charges immediately. Restarting consumes
the whole click; a separate press is required to charge the next jump.

The game includes seven procedural platform designs, center streaks, four idle
rewards, character flips, landing particles, camera easing, ascending
charge notes with natural release, a traditional Christmas music-box melody
with floating notes, distinct platform feedback
and an App-private best score. Geometry and sound recordings are authored in this project;
no external game assets or engine are needed.

## Build and run

Activate ESP-IDF 6.1 and the repository's WASI SDK / pinned WAMRC toolchain as
described in the [flashing guide](../../../docs/development/flashing.zh-CN.md).

```sh
python3 tools/micropixel package guest/apps/jump-jump \
    --aot-target riscv32-ilp32f --profile release

# Identify the S31 by MAC before selecting DEVICE_PORT; close other serial tools.
python3 tools/micropixel --transport usb --port "$DEVICE_PORT" \
    run guest/apps/jump-jump --profile release --no-follow

# Optional stage timing, emitted every 120 rendered frames.
python3 tools/micropixel --transport usb --port "$DEVICE_PORT" \
    app start micropixel.jump-jump -- --perf
```

Output is `build/apps/jump-jump/jump-jump.bundle.bin`. This App checks for the
polygon capability at runtime (the published Graphics service version remains
1.0). Screens with both dimensions at least 240 pixels are supported, including
320×240, 480×480 and 720×720. Geometry uniformly fits a centered square using
the shorter screen dimension; the background fills the entire panel. Text uses
native device fonts and compact hints on small screens. Native touch coordinates
cover the entire screen, including the extra space beside the square playfield.
Physics, charge timing and collision tolerances do not depend on resolution. It is included in firmware 0.9.3 factory images. It uses the public SDK without changing
the Host, ABI or WAMR dependency.

## Gameplay contracts

- Charge distance is derived from input timestamps, saturating after 1.2 s.
  Flight and landing use elapsed time, independent of rendered frame count.
- Safe landing gives one point. Consecutive center hits give 2, 4, 6, … up to
  32 points each. A non-center landing breaks the streak. A short hop back
  onto the same platform gives no points and breaks the streak.
- Stand idle on a drain / cube / shop / record platform for two seconds to
  earn 5 / 10 / 15 / 30 points. Each platform rewards once. Charging suppresses
  the reward; cancelling starts a fresh idle interval.
- Ordinary platforms dominate. Special scenery slots are separated by five
  successful jumps (six above 1,000 points); only four of 17 slots give idle
  rewards. The round pedestal table belongs to the ordinary pool and can appear early.
  Ordinary platforms shrink progressively, including much smaller tables. Other
  special scenery currently falls back to plain art without bonus points.
  Selection approximates the historical pool; it does not recreate its full
  30-model inventory or exact random sequence.
- Square and circular collision shapes match the platform tops. Partial foot
  support topples the character; it does not silently snap onto the platform.
- One touch or key owns a charge. Other contacts and key repeats cannot replace
  or release it. Cancel/Resume clears the held input without launching a jump.
- The route is deterministic for a model seed, reuses three platform slots and
  rebases coordinates at every landing. The draw buffer and sound queue have
  fixed capacity. Best score is saved on game over and Stop, not each frame.

Current charge, collision, flight, reward timing and the 32-point streak cap
are the version 0.1 rules. They have not been calibrated frame by frame against
a specific WeChat release. This is an independently implemented first playable
version, not a claim of exact original physics or sound reproduction.

## Source

| File | Responsibility |
| --- | --- |
| `model.*` | Time-based simulation, collision, generator, score, input ownership |
| `renderer.*` | Orthographic geometry, palette, animation and bounded draw buffer |
| `main.cpp` | Event loop, SDK resources, raster adapter, storage and sound scheduling |
| `audio/sfx.json` | Sole source of tone and recorded-track parameters |
| `audio/build_tracks.py`, `assets/*.ogg` | Reproducible synthesized drain and music-box source assets |
| `assets/launch.jpg` | 720×720 launcher cover, referenced by `launch_asset` |
| `assets/source/cover-v1.prompt.txt` | Tracked image-generation prompt; original `cover-v1.png` stays local and is not required to build |
| `store/*.md`, `store/summary.txt` | Store description and release notes; screenshots stay local |

The target is 30 presented frames/s at native resolution. Each Host buffer is
fully cleared once, then keeps its own bounded 32-band coverage map. Reuse clears only the previous content in that particular buffer
through RECT records before repainting the current geometry and text. This
preserves native resolution and avoids rewriting untouched background pixels.
Coverage includes offscreen clipping, subpixel edges and text margins; it is
independent of which buffer was most recently presented. Panels with either
dimension above 480 use three buffers by default; smaller panels use two. A
failed third-buffer allocation falls back to two. `--buffers=2` and
`--buffers=3` override the choice for A/B measurements, and `--perf` reports
the actual count along with Guest geometry, Host raster/record submission,
present-call time and skipped frames. At 720×720, the third native RGB565
buffer costs 1,036,800 additional bytes before alignment. These are stage measurements, not a guarantee of panel latency.
`--seed=123` fixes the initial route RNG for reproducible input replays; each
restart derives a fresh deterministic seed from that stream.

## Validation

```sh
bash tools/tests/test_firmware_host.sh
python3 -m unittest tools.tests.test_analyze_sfx -v
bash tools/check_firmware_style.sh --format-only
git diff --check
```

The native `jump_jump` suite runs through the Host test wrapper. It covers
1,000-jump deterministic replays at different frame intervals, circle/square
edges, misses, same-platform hops, streaks, all special rewards, input ownership
and bounded finite render geometry. Adaptive layout tests cover ready, charge,
flight, landing, results and restart at 320×240, 480×480, 720×720 and 240×320.
The actual Host polygon kernels also compare incremental background clearing
against full redraw pixel by pixel across those sizes, camera movement, frame
buffer reuse, effects and menu transitions. Text coverage is checked with
synthetic glyph bounds; real font appearance still requires device screenshots.

Set `JUMP_JUMP_PREVIEW_DIR="$PWD/build/apps/jump-jump/preview"` when running the
wrapper to export SVG frames from the same renderer for visual inspection.
Preview text uses approximate system-font metrics; device screenshots remain
the authority for appearance.

On S31, check native-resolution frame pacing, press/release response, fast
restart, pause while holding, session teardown, saved scores and audio A/B
listening at the same system volume as Snake. See the
[audio notes](audio/README.md). Keep raw logs and measurements in `build/`.
