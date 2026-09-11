# Tomb Explorer

Third-person exploration demo of the PS1-style polygon path: rooms joined by portals, a low-polygon
explorer, sector-based collision and a follow camera, drawn with the SDK `MeshRenderer` over the
Host `TRIANGLE`/`QUAD` raster records. Every asset is generated in this directory; nothing is taken
from any commercial game.

## Layout

| Path | Role |
|---|---|
| `main.cpp` | frame loop, options, HostSurface + palette/texture upload, stats |
| `world/level.hpp` | read-only level contract (rooms, sectors with corner heights, portals) |
| `world/level_data.cpp` | generated from `tools/level.json` by `tools/generate_level.py` |
| `world/room_world.*` | portal traversal (visible rooms, draw order, scissor rectangles) and floor/ceiling queries |
| `game/player.*` | walking, steps, drops, jumps, wall sliding, room changes, camera placement |
| `game/character.*` | eleven-box explorer with procedural walk/jump poses |
| `gfx/palette.*` | 16 ramps × 16 steps INDEX8 palette and the 16-level lit palette |
| `gfx/textures.*` | generated 64×64 textures (`tools/generate_textures.py`) |
| `input/touch_controls.*` | left-half stick, right-half orbit/tilt drag, tap to jump |

## Level description

`tools/level.json` lists rooms as sector grids. Each room has an origin, a base floor and ceiling,
an ASCII `map` (`#` solid, `.` floor, `0-9` floor raised by 0.25 per digit, `~` water), optional
`slope`, point `lights` baked into vertex brightness, texture overrides, an optional `frieze`
height above which walls use the carved texture, and `portals` (`side`, `range` of boundary
sectors, `to`). Portals must be declared from both rooms and meet open sectors on both sides; the
generator checks this, emits floor/ceiling/wall/step quads split so no quad spans 256 texels, and
keeps each room under `MeshRenderer::kMaxMeshVertices`.

```bash
python3 guest/apps/tomb-explorer/tools/generate_textures.py   # gfx/textures.{hpp,cpp}
python3 guest/apps/tomb-explorer/tools/generate_level.py      # world/level_data.cpp
python3 guest/apps/tomb-explorer/tools/generate_level.py --check
```

## Run

```bash
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX run guest/apps/tomb-explorer \
    --aot-target riscv32-ilp32f --profile performance
# scripted route with stats every 120 frames
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX run guest/apps/tomb-explorer \
    --aot-target riscv32-ilp32f --profile performance -- --benchmark
```

Options after `--`: `--benchmark` (fixed 30 Hz step along a route through every room, logs
`tomb-bench:` lines), `--perf` (same stats while playing), `--upscale=N` (render N times smaller;
panels wider than 480 px default to 2).

A stats line reads `fps_x100 render_avg_us render_max_us present_avg_us wait_avg_us rooms faces
culled polygons subdivided overdraw_x100 dropped room`. `render_avg_us` includes the Host kernels
(they run on the Guest task inside `HostSurface::Update`); `overdraw_x100` is the MeshRenderer's
screen-area estimate of the queued polygons over the buffer area; `dropped` must stay 0 or the
polygon pool in `main.cpp` needs to grow.

## Tests

`tools/tests/test_tomb_room_world.cpp` (run by `bash tools/tests/test_firmware_host.sh`) checks the
generated level, portal traversal order and scissors, sector heights and player collision on the host.
