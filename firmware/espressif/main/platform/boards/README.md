# MicroPixel board port checklist

> Board initializes Drivers and registers its available Peripherals,
> Controllers, and Presentation with Platform. Platform turns Peripherals into
> public Devices, which are managed by Services and exposed to Guests through
> ABI Endpoints.

中文：Board 初始化 Driver，把可用 Peripheral、Controller 和 Presentation 登记给 Platform；Platform 将
Peripheral 转成公开 Device，由 Service 管理，并通过 ABI Endpoint 提供给 Guest。固定词义和命名规则见
[`docs/design/firmware-terminology.zh-CN.md`](../../../../../docs/design/firmware-terminology.zh-CN.md)。

`boards/<board>/` is a Platform composition layer. A new board begins with a
board name and registers only hardware that has actually initialized. It owns
only pin mapping, peripheral composition, power sequencing, board metadata and
implementations that cannot be reused. `Platform::Publish()` fills missing
services, owns public device identity and assembles shared engines, so a port
can grow incrementally without board-local placeholders.

Keep a new board flat by default. Put a single board-specific implementation,
such as an audio sink, power key, battery Peripheral or haptic actuator,
directly under `boards/<board>/`; do not pre-create capability directories.
Create a named subdirectory only after that area becomes a real subsystem with
multiple implementation units or substantial private machinery. For example,
the current product boards keep `display/` because framebuffer/GRAM handling,
transition composition, brightness and capture form an independent hardware
presentation subsystem.

Keep `platform.cpp` as the narrow composition root: it owns initialization
order, Peripheral/Controller/Presentation objects and registration. A square-display board embeds one
`host/ui/lvgl/square_common/SquareSystemUiState` in its private
`platform_state.hpp`; it must not duplicate LVGL pages, Hall arrays, pointer
routing, theme state, startup/launch lifecycle or Guest foreground policy. The
board selects one complete 480/720 profile instead of assembling UI fields. Board-local
`presentation.*` keeps only framebuffer/GRAM ownership, Hall transition
composition, screen capture, display refresh and other hardware presentation
interfaces; it consumes a Host-computed transition request and does not read Hall
indices, scroll state or UI objects. The bounded virtualized Hall-card policy lives under `host/ui/lvgl/`; a
board must not define pages, Hall lifecycle, card events or product UI
properties.

## Required files and registration

1. Add `boards/<board>/CMakeLists.txt` and the implementation that provides the
   single `micropixel::platform::ConfiguredBoard()` symbol.
2. Add one `MICROPIXEL_BOARD_<NAME>` entry to the `MICROPIXEL_BOARD` choice in
   `main/Kconfig.projbuild`.
3. Select the board CMake file in `platform/CMakeLists.txt`; include only the
   shared layers that the board uses. The board CMake file explicitly lists
   its reusable `drivers/` and `input/` sources so selecting one board never
   compiles another board's hardware set.
4. Add a board `sdkconfig` defaults file. Keep compile-only profiles in a
   separate build directory and never expose a flash command for them.
5. Add one declarative entry to `tools/firmware_profiles.json`. Board shell
   scripts are aliases for product workflows; they must delegate ESP-IDF
   build, flash, monitor and chip-safe port selection to `tools/firmware.py`.

The ESP-Mosaico P0/P1 profile proves ESP32-S31 target selection, WAMR/AOT
configuration, 16 KiB MMU-page-safe BundleFS, native Wi-Fi, CO5300 display,
CST9217 interrupt-driven touch, BQ27220 battery, ES8311/NS4150B audio and the
digital vibration motor. It reuses the shared App Hall, Status Layer,
fixed-capacity audio engine, logical-coordinate/layout profiles, transition timeline and
PPA/DMA2D primitives. Both physical boards also use the same Graphics contract
forwarder and `SquareSystemUiState`; launch screens, Hall bookkeeping, system
pages and Host pointer routing are shared. The board layer owns only pin mapping, power sequencing,
codec/I2S output and the RGB565/QSPI presentation boundary. Codec control,
battery and touch work are serialized through the board's shared I2C executor.
Brightness uses the CO5300 component API instead of issuing panel registers
from System UI.

BMI270 and both BMM150 devices use the pinned Bosch SensorAPI sources under
`components/bosch_sensorapi/` and reusable drivers under `platform/drivers/sensors/`.
Configuration and sampling are serialized through the board's shared I2C executor;
only successfully initialized acceleration, angular-velocity and magnetic-field
channels are registered. Axis mapping and magnetic calibration still require
hardware validation and must not be guessed from Metalio-Claw4. The POWER switch,
Function button, status LED, battery refresh and explicit light-sleep/power-off path
are present; NAND App Store and module discovery remain P2.

Every Board implementation submits one `BoardRegistration`. The minimum is
only `device::BoardInfo{.board = "..."}`; add Graphics/Input/System UI when
the display path is ready, then add audio, battery, Wi-Fi, power and external
devices independently. Audio silicon implements `AudioOutputPeripheral`; the shared
`AudioEngine` owns mixing and public Audio behavior. A board with accelerated
Guest↔Hall presentation implements the complete `DisplayTransition` interface;
a board without it returns no transition interface.

Sensors, GPIO and haptics are Peripherals with board-local Channel values. The
board never assigns a public `DeviceId`, wire kind or capability bits. It calls
`AddSensor`, `AddGpio` or `AddHaptics` with its Peripheral, local Channel and a
physical display name; `DeviceRegistry` owns enumeration and public IDs. GPIO
names may follow the hardware manual (`P15`, `GPIO15`, `IO15`, and so on).
Names are descriptive only: Peripheral routing uses the local Channel and Guest
routing uses the upper-assigned opaque ID.

Before adding board-local code, check these homes:

- reusable audio, haptics or Wi-Fi implementation: its named `platform/<domain>/` directory;
- narrow interface adapter: `platform/adapters/`;
- shared bus scheduling: `platform/buses/`;
- controller or peripheral driver: `platform/drivers/<kind>/<chip>/`;
- reusable touch-to-Input implementation: `platform/input/`;
- reusable display engine, Guest renderer or LVGL input bridge:
  `platform/lvgl/`;
- hardware-independent App Hall, Status Layer or system page:
  `host/ui/lvgl/`;
- hardware-independent contract: `device/`.

Run at least the Host regression suite, architecture check, format check and
the new board's full ESP-IDF build. Keep `bash tools/p4.sh build-null` passing.
For the preview S31 target use `bash tools/s31.sh build-null` and
`bash tools/s31.sh build-host`. `s31-null` cannot be flashed; the physical
bring-up profile exposes `flash-host` and `monitor` with mandatory S31 chip
verification.
