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

## Display configuration and texture scaling (0.19.0)

Unconfigured apps use native screen pixels. Configure a design canvas before querying layout,
creating scenes, loading textures, or translating touch input:

```cpp
app.renderer().ConfigureDisplay({
    .logical_size = {320U, 240U},
    .scale_mode = micropixel::DisplayScaleMode::kAspectFit,
}).value();
auto scene = app.renderer().CreateScene().value();
auto texture = app.resources().LoadTexture(atlas_asset).value();
```

| Mode | Behavior |
| --- | --- |
| `kNative` (default) | Native pixels; logical_size must be empty |
| `kAspectFit` | Center a fixed canvas, preserving aspect ratio; clip contents to the viewport and fill bars with scene background |
| `kAspectFill` | Center and crop a fixed canvas to cover the screen |
| `kExpand` | Fit the design size and expand logical dimensions to match the screen aspect ratio |

Non-native modes require nonzero dimensions. Logical and viewport extents must not exceed 32767.
First use freezes the configuration; subsequent ConfigureDisplay calls return kInvalidState.
Invalid configuration leaves the previous one intact. Explicit SceneDescriptor dimensions must match
this configured canvas. RendererInfo exposes logical layout/safe insets and the actual physical screen size.
Touch uses the inverse transform, including viewport offsets. Letterbox touches stay outside the canvas;
do not assume all events lie inside it. Safe insets include cropped portions and exclude letterboxing.
System fonts retain Host-selected physical sizes; MeasureText converts their metrics to logical units.

LoadTexture defaults to `TextureScale::kConfigured`, following the display scale (1:1 when unconfigured).
Explicit `kNative` preserves authored pixels; `kDisplay` follows the same configured scale.
`TextureLoadOptions::Ratio(n, d)` overrides it. Ratios must be positive, with reduced numerator and
denominator at most 4096. Scaling can enlarge images; it does not automatically respond to free memory.
Texture width/height and Scene atlas source rectangles remain in authored coordinates.

### DirectSurface

Surface buffers and raster commands always use buffer pixels, independent of ConfigureDisplay.
Creating a surface does not change the default texture scale. For a design canvas with short edge 320:

```cpp
auto surface = app.renderer().CreateHostSurface(2U, 2U).value();
auto scale = micropixel::TextureLoadOptions::ForShortEdge(
    320U, surface.buffer_width(), surface.buffer_height());
auto texture = app.resources().LoadTexture(atlas_asset, scale).value();
```

On a 480×480 screen this creates 240×240 buffers and loads at 240/320 scale.
The reference is the design canvas short edge, not the atlas size. Native pixel assets should explicitly
use kNative. Upscale must divide both physical screen dimensions exactly. Recreating a surface does not
reload textures. The compatibility kSurface entry point uses configured display scale / active surface
upscale and fails without an active surface. A mapping object is not required.
Touch remains in the configured application coordinates; Surface apps must convert it to buffer coordinates.
Use `surface.ToBuffer(Point/Rect)` and `surface.ToLogical(Point)` to share the configured viewport, including
offsets and upscale. These methods do not clip coordinates and return empty geometry for an invalid surface.
`surface.texture_scale()` derives the configured display scale divided by upscale; an invalid surface returns
an invalid ratio and texture loading fails. No mapping object or hand-written conversion is required.

### Migration from 0.18

Existing Bundles retain their compiled Guest Runtime and behavior; the ABI is unchanged.
When rebuilding a Scene app designed around 720, explicitly configure `{720, 720}` and kExpand.
Remove redundant kDisplay arguments; mark native art variants kNative. Surface apps must audit old
720-based input conversion. Manifest display requirements remain installation filters with their existing
semantics; they do not configure runtime coordinates.
See [Snake](../apps/snake/snake_app.cpp), [Tilt](../apps/tilt/tilt_app.cpp), and
[the atlas demo](../apps/sdk-demo/pages/resource_atlas_demo.cpp).

### PNG memory

The Host still fully decodes PNG before scaling. Downscaling reduces retained texture memory but does not
remove the full-resolution decode peak; source and destination coexist while scaling. An alpha image
usually needs width × height × 4 bytes before stride and decoder overhead. Split large atlases and release
unused resources. Streaming decode/downsampling is not implemented by this SDK change.
