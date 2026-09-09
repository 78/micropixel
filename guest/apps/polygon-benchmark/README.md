# Polygon Benchmark

Measures the Host raster `TRIANGLE`/`QUAD` kernels (Graphics 1.6 polygon capability) on a real
board. It is the acceptance spike for the TR1-style polygon path: the numbers decide whether a
480×480 scene at ~1.5× overdraw fits in 20 fps or needs `--upscale=2` / portal scissors.

## Run

```bash
# ESP-Mosaico (S31), default cycle of every phase, 240 frames each
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX run guest/apps/polygon-benchmark \
    --aot-target riscv32-ilp32f --profile performance -- --frames=240

# Stay in one phase, render at half resolution enlarged by the Host
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX run guest/apps/polygon-benchmark \
    --aot-target riscv32-ilp32f --profile performance -- --phase=quads-1.5x --upscale=2
```

Options after `--`: `--upscale=N` (1..4), `--phase=NAME`, `--frames=N` (frames per phase, ≥120).

## Phases

| Phase | Content | What it tells |
|---|---|---|
| `fill` | one buffer-sized textured quad | pure fill cost in ns/pixel |
| `quads-1x` | random 120 px textured Gouraud quads, 1.0× buffer area | baseline overdraw |
| `quads-1.5x` | same at 1.5× | the budgeted overdraw for a room scene |
| `quads-2x` | same at 2.0× | headroom |
| `small-2x` | 24 px quads at 2.0× | per-polygon setup overhead |
| `room` | closed 8×4×8 box through `MeshRenderer`, spinning camera | Guest geometry + sorting + Host fill together |

## Reading the log

Every 120 frames:

```
polygon-bench: phase=quads-1.5x frames=120 fps_x100=2210 render_avg_us=38210 present_avg_us=310 wait_avg_us=6800
    polygons=214 pixels=345600 overdraw_x100=150 ns_per_pixel=110
```

- `render_avg_us` is the Guest-side time of `HostSurface::Update`, which runs the Host kernels on the
  Guest task, so `ns_per_pixel = render / pixels` is the kernel cost including record validation.
- `wait_avg_us` is time blocked on a free buffer (panel bound when large).
- In `room`, `pixels` is the MeshRenderer's screen-space estimate (plus one clear), `subdivided` the
  faces split for the affine warp, `dropped` polygons lost to a full pool (must stay 0).

Acceptance for the polygon path: `quads-1.5x` at 480×480 ≥ 20 fps (`fps_x100 ≥ 2000`). Below that,
compare the same phase with `--upscale=2` before building content that relies on full resolution.
Keep captured logs out of the repository.

## ESP-Mosaico (S31) sample, 480×480, performance profile, Direct Surface

| Phase | fps | render µs | Host ns/px (quad) | overdraw |
|---|---:|---:|---:|---:|
| `fill` | 40 | 14.3 | 60 | 1.00× |
| `quads-1x` | 40 | 15.1 | 83 | 0.75× |
| `quads-1.5x` | 39.5 | 22.6 | 83 | 1.14× |
| `quads-2x` | 33.2 | 29.5 | 82 | 1.50× |
| `small-2x` | 16.5 | 59.5 | 145 | 1.53× |
| `room` | 28–29 | 33–35 | 91 (quad) / 108 (tri) | 1.99× |

`quads-1.5x` clears the gate with headroom; full-resolution content does not need `upscale=2`.
`small-2x` shows setup cost dominates when many tiny quads share the same pixel budget — keep room
meshes coarse and rely on portal scissors rather than micro-tessellation.
