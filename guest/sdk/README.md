# MicroPixel C++ SDK

A restricted C++23 SDK for WebAssembly apps. No ESP-IDF, LVGL, board-specific types, threads, exceptions, or RTTI.

## Start here

- [Quickstart](QUICKSTART.md) — create and run an app.
- [Publishing](PUBLISHING.md) — distribute an app through the store.
- [Windows automation](AI.md) — managed installation and JSON commands.
- [API reference (中文)](README.zh-CN.md) — resources, events, graphics, audio, input, and devices.

## Programming model

Apps run a single-threaded event loop. The Host owns hardware, system UI, and app lifecycle.
Service views provide access to capabilities; resource handles use move-only ownership.
Fallible operations return `Result<T>`.

Use `Scene` for object-based UI and 2D graphics. `HostSurface` submits raster commands for Host rendering;
`GuestSurface` accepts pixels rendered by the app. Presented buffers cannot be reused until released.

## Examples

| App | Demonstrates |
|---|---|
| [SDK Demo](../apps/sdk-demo/README.zh-CN.md) | Services and graphics |
| [Snake](../apps/snake/README.zh-CN.md) | 2D game and audio |
| [Juicy Tilt](../apps/tilt/README.zh-CN.md) | Sensor input and physics |
| [Maze Break](../apps/maze-evil/README.zh-CN.md) | Raycasting |
| [Tomb Explorer](../apps/tomb-explorer/README.md) | Polygon rendering and portals |

[ABI reference (中文)](../abi/README.zh-CN.md)

## Mode7 and surface textures

`Mode7Plane` converts a perspective ground plane into one `Span` per screen row.
The app supplies curvature, texture-row selection, object ordering, and HUD content.
Use a compatible Host supporting Span records; the basic raster capability flag alone does not establish support.

### Frame setup

Include `sdk/mode7_plane.hpp`. Initialize the viewport from the surface's `buffer_width()` and `buffer_height()`.
Upload row-major INDEX8 textures and a lighting palette through `RasterResources`.
Stop using the plane if `Initialize(config)` returns false.

Inside a `HostSurface::Update()` callback:

```cpp
plane.PlaceRows([](float depth) { return 0.0F; }, camera_x, road_half_width);
for (uint32_t i = 0; i < plane.row_count(); ++i) {
    plane.rows()[i].style = 0;
}
if (!plane.Draw(list)) {
    return false;
}
```

`style` selects a texture row. `Project()` returns buffer coordinates; its visibility flag only checks depth.
Sort other objects from far to near. Mode7 does not create a Z-buffer.
Follow the surface's acquire/present/release lifecycle before reusing buffers.

### Textures

`TextureScale::kNative` preserves asset dimensions; `kDisplay` adapts to the logical canvas.
`kSurface` uses display scale divided by the active DirectSurface upscale factor.
Create the surface before loading `kSurface` textures. Recreating it does not reload textures automatically.

Matching source and destination dimensions avoids resampling. The Host may accelerate eligible opaque RGB565 Image
copies on P4/S31; S3 uses the CPU. Keep canonical texture byte order on the Guest side.
