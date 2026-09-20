# MicroPixel C++ SDK

A restricted C++23 SDK for WebAssembly apps. No ESP-IDF, LVGL, board-specific types, threads, exceptions, or RTTI.

## Start here

- [Quickstart](QUICKSTART.md) — create and run an app.
- [Publishing](PUBLISHING.md) — distribute an app through the store.
- [Windows automation](AI.md) — managed installation and JSON commands.
- [API reference (中文)](README.zh-CN.md) — resources, events, graphics, audio, input, and devices.

## Upgrading to 0.20.0

Use firmware 0.9.3 for the companion Host input changes. Existing Bundles remain installed during a
Host-only update; changing the factory preload list affects full images only.

Apps migrating to `app.gamepad()` should configure it once and consume its state each frame; remove
manual forwarding of the same events to the pad. Disable the gamepad while an app menu owns touch input.
Touch and key controls remain usable on earlier Hosts; analog axis events require Input 1.1 support.
Check the capability catalog below before keeping app-local math, random, pool, sound or sensor helpers.

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

## Capability catalog

Check this table before writing a helper inside an app. Every row is a header under `guest/sdk/` that
already covers the need; apps must not re-implement these locally.

| Need | Use | Header |
|---|---|---|
| Event loop, service views, `Result<T>` | `Application`, `app.xxx()` | `application.hpp`, `result.hpp` |
| Retained 2D UI, sprites, labels, layout | `Scene`, `SpriteBatch`, `ui::FlexContainer`, `ui::TextButton` | `scene.hpp`, `ui/*.hpp` |
| Host-rasterized frames, INDEX8 textures, palettes | `HostSurface`, `RasterDrawList`, `RasterResources` | `graphics.hpp` |
| Raycast walls, PS1-style polygons, Mode7 ground, globe | `Raycaster`, `MeshRenderer`, `Mode7Plane`, `SphereView` | `raycast.hpp`, `mesh_renderer.hpp`, `mode7_plane.hpp`, `sphere_view.hpp` |
| Textures, fonts, dynamic textures | `Resources::LoadTexture/LoadFont/CreateDynamicTexture` | `resources.hpp` |
| Logical/buffer coordinate mapping for surfaces | `DirectSurface::ToBuffer/ToLogical` | `graphics.hpp` |
| On-screen stick, look pad and buttons; physical gamepad keys and axes | `app.gamepad()`, `GamepadSkin` | `gamepad.hpp`, `gamepad_skin.hpp` |
| Button press/release with hit padding | `ui::Button` | `ui/button.hpp` |
| Multi-note sound effects from `audio/sfx.json` | `ToneSequencer<N>`, `ToneSpec::ToTone` | `tone_sequencer.hpp`, `audio.hpp` |
| Clips, PCM streams, tones | `Audio::Play/Load/OpenPcmStream` | `audio.hpp` |
| Sin/Cos/Atan2 without libm, clamp, lerp, smoothstep, deadzone | `math::*` | `math.hpp` |
| Reproducible random numbers (seeds, replays, tests) | `XorShift32` | `random.hpp` |
| Hardware random numbers | `Random::U32/Below` | `random.hpp` |
| Fixed-capacity particle/trail/popup pools | `CyclicPool<T, N>` | `cyclic_pool.hpp` |
| Bounded strings, integers and decimals | `FixedString<N>`, `AppendFixed` | `fixed_string.hpp` |
| Rectangle overlap and union (dirty regions) | `Rect::intersects/united/intersection` | `geometry.hpp` |
| Timers and frame ticks | `Timers::After/Every`, `TimerEvent::delta()` | `timer.hpp` |
| Accelerometer, gyroscope, magnetometer | `Sensors::OpenFirst<Acceleration>(devices, interval)` | `sensors.hpp`, `sensor_types.hpp` |
| Tilt-to-steer: calibration, low-pass, deadzone | `TiltFilter` | `tilt_filter.hpp` |
| Persistent scores and settings | `KVStore::GetU32Or/SetU32/GetBytes` | `storage.hpp` |
| Launch flags and numeric options | `LaunchArguments::HasFlag/GetUnsigned/FindValue` | `launch_arguments.hpp` |
| Locale-aware strings | `Localization::CurrentLocale` + generated string tables | `localization.hpp` |
| Haptics, GPIO, power, device discovery | `Haptics`, `Gpio`, `PowerInfo`, `Devices` | `haptics.hpp`, `gpio.hpp`, `power_info.hpp`, `devices.hpp` |

## Game helpers

`math.hpp` is freestanding: `Sin`, `Cos`, `Atan`, `Atan2`, `WrapAngle`, `ApproachAngle`, `Sqrt`, `Floor`,
`Clamp`, `Lerp`, `SmoothStep`, `ApplyDeadzone`. `XorShift32` replays identically from a seed; `Random`
stays the source of entropy. `CyclicPool<T, N>` hands out slots in order and overwrites the oldest one.

`TiltFilter` turns accelerometer samples into a -1..1 screen-space tilt (neutral calibration, exponential
low-pass, deadzone, axis inversion); open the sensor with `app.sensors().OpenFirst<Acceleration>(app.devices(), 10_ms)`
and feed `Sample(value, timestamp)` per reading. `FixedString::AppendFixed`, `LaunchArguments::HasFlag/GetUnsigned`,
`KVStore::GetU32Or` and `Rect::intersects/united` cover the small utilities apps used to hand-write.

`ToneSequencer<N>` plays the `ToneSpec` arrays the build generates from `audio/sfx.json`:

```cpp
micropixel::ToneSequencer<8> tones{app.audio(), audio_available};
tones.Play(my_sfx::kJump);          // game event; delayed notes wait in N slots
tones.Advance(tick.delta());        // frame timer
tones.StopAll();                    // pause or game over
```

## Gamepad

`app.gamepad()` is a Runtime-owned gamepad. Games declare the logical controls once; the Guest Runtime then
feeds every touch, key, analog axis (Input 1.1), gamepad device and Resume event to it while decoding events,
so game code never routes input and never branches on the source:

```cpp
const micropixel::GamepadButtonConfig buttons[] = {{.glyph = micropixel::GamepadGlyph::kFire}};
app.gamepad().Configure({.layout = micropixel::GamepadLayout::kStickLookButtons,
                         .buttons = buttons});
micropixel::GamepadSkin skin;
skin.Initialize(app.resources(), app.gamepad().pad());     // bakes ring, knob and buttons into one texture

// frame
const micropixel::GamepadState state = app.gamepad().Consume();  // stick_x/y, look_dx/dy, Held/Pressed/Released,
                                                                 // right_x/y and triggers from a physical pad
skin.Draw(list, app.gamepad().pad());                     // HostSurface; or skin.Attach(scene) + skin.Sync(pad)
```

Omitted `bounds` (or `{}`) covers the current logical canvas. Configure the display or create the surface
before configuring the gamepad; set `bounds` explicitly only for a custom region.

Events the gamepad took are marked `Event::gamepad_handled()`, so menu code can skip them; call
`app.gamepad().set_enabled(false)` on pages where touches must reach the App's own UI. Layouts: `kStickOnly`,
`kStickLook` (tap on the look pad presses `look_tap_button`), `kStickButtons`, `kStickLookButtons`,
`kDPadButtons` (directions snapped to -1/0/1), `kButtonsOnly`. Buttons are named by physical position
(`kSouth` is the primary action) and map 1:1 to `KeyCode::kSouth..kNorth`; `kConfirm` is South, `kUp..kRight`
and the left stick axis drive the stick (a touching finger wins, then the axis, then keys). Each contact keeps
its role until it lifts. The overlay follows `GamepadOverlayPolicy`: `kAuto` hides it after key/axis input or a
gamepad connection until the next touch. `physical_connected()` reports a connected gamepad device.

`GamepadConfig::buttons` accepts up to four `GamepadButtonConfig` values in South/East/West/North order.
`Configure` copies the descriptors, so the source array or vector need not outlive the call. Each button
sets its glyph, optional centre/radius and `GamepadButtonStyle`; omitted geometry uses the layout preset.
Centres use `bounds` coordinates and circles must fit inside the bounds. Hit areas add 25% padding;
overlaps prefer the first button. Reconfigure the pad and reinitialize its skin after changing buttons.
Views from `pad.buttons()` and `pad.config().buttons` remain valid until reconfiguration.

Standalone `VirtualGamepad` requires explicit `bounds` and accepts `OnEvent(event)`. Custom renderers can read
`stick_geometry()` and `button_geometry()`. `GamepadSkinStyle` controls the stick and overlay; button
styles belong to their descriptors. Defaults use matching faint rims, grey glyphs, transparent idle
buttons and dark grey press feedback. Floating sticks appear only while engaged unless
`show_stick_at_rest` is enabled; fixed sticks stay visible.

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
Surface-only apps get one coordinate space: when no `ConfigureDisplay` was called and no Scene exists, the
first DirectSurface adopts its buffer size as the logical canvas (like Godot's viewport stretch or SDL3's
logical presentation). Touch then arrives in buffer pixels, `RendererInfo::width()/height()` report the buffer,
`ToBuffer` is the identity and the canvas stays frozen for later surfaces. Reading `renderer().info()` first to
pick the upscale is fine. Mixed apps that configure a design canvas or show a Scene before the surface keep
their logical space; there touch stays in logical coordinates and `surface.ToBuffer(Point/Rect)` /
`surface.ToLogical(Point)` convert, including offsets and upscale. These methods do not clip coordinates and
return empty geometry for an invalid surface. `surface.texture_scale()` derives the display scale divided by
upscale (1:1 for an adopted canvas); an invalid surface returns an invalid ratio and texture loading fails.

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
