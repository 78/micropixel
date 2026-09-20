# MicroPixel board port checklist

> Board initializes Drivers and registers its available Peripherals,
> Controllers, and Presentation with Platform. Platform turns Peripherals into
> public Devices, which are managed by Services and exposed to Guests through
> ABI Endpoints.

中文：Board 初始化 Driver，把可用 Peripheral、Controller 和 Presentation 登记给 Platform；Platform 将
Peripheral 转成公开 Device，由 Service 管理，并通过 ABI Endpoint 提供给 Guest。固定词义和命名规则见
[架构与术语](../../../../../docs/design/architecture.zh-CN.md#platform-术语)。

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

## Metalio-Claw4 peripheral power

`BoardIo::InitializeIoExpander()` establishes the factory TCA9555 startup
directions and rail levels before display and peripheral initialization. NT26,
the Bluetooth audio bridge and the active-low SD rail are powered; GPS, camera,
PA and USB host power are disabled. The audio source selects the Bluetooth
bridge. The power-key pulse stays low, while the power key, accelerometer
interrupt, wired/wireless charge detectors and unused lines stay inputs.
Output latches are written before enabling their output directions to avoid
transient PA/USB host activation. Subsequent audio and power-key operations
continue through the shared I2C executor.

These power defaults do not by themselves implement SD mounting, camera capture
or GPS. Hardware acceptance must check rail levels after cold boot and MCU reset,
and verify charging/key inputs.

## Metalio-Claw4 cellular network

`CellularController` manages the factory NT26 UART Ethernet driver: UART1 at
2,000,000 baud, TX28/RX29, MRDY13/SRDY4 and TCA9555 P0.7 power/reset. The default
PDP context is the factory `IP` / `eapn1.net`. SIM selection is exposed by the
4G settings page; an APN editor is not implemented.

Wi-Fi and cellular have independent persisted switches and may both be enabled,
or both disabled. Wi-Fi is always initialized, even on devices previously saved
in cellular-only mode. The existing `network/type` key is retained as the cellular
enable flag (0 off, 1 on); `host_wifi/state` continues to store the Wi-Fi switch.
Absent cellular settings leave it off. Switching either radio does not reboot.
Cellular start/stop runs on the existing background executor; persistence or driver
failures leave a retryable error, and a failed stop never cuts power to live workers.
SIM-slot changes quiesce PDP control, apply the RF/slot commands, then restart only
the modem after a 100 ms reset pulse. The Host and app session remain running.

ESP-NETIF automatically chooses Wi-Fi STA (priority 100) over cellular Ethernet
(priority 50), falls back when Wi-Fi disconnects, and returns to Wi-Fi after DHCP.
The Claw4 profile enables per-interface DNS so resolvers follow the default route.
Remote Control rebuilds its QUIC connection when the preferred transport changes;
ongoing requests may need retrying. This is link/IP failover, not an Internet
health check: a Wi-Fi AP with working DHCP but a broken WAN remains preferred.

The Host reads cellular availability, mode, IP connection and measured CSQ bars.
CSQ uses factory thresholds (0–9, 10–14, 15–19, 20–31; unknown is no bars) and is
queried after connection and at most every five seconds while the system UI
requests refresh. Remote control, App Store, OTA and SNTP accept cellular-only
connectivity. Other boards retain an unavailable cellular capability.

Power-off cancels queued mode switches and stops the modem before cutting board
power. Manual sleep stops the driver and disables its rail; wake starts the saved
cellular mode again. A failed stop rejects sleep instead of releasing live driver
storage. Physical power-key wake remains the board's explicit-sleep policy.

Host tests use fake UART, NVS and I2C dependencies to exercise the real board
controller. Target acceptance remains pending: cold boot with either/both radios, independent switch
persistence, Wi-Fi loss/recovery with 4G connected, both-off behavior, actual SIM/APN registration, DHCP, signal changes, remote/store/OTA
over 4G, no-SIM recovery, and shutdown/sleep during traffic. Neither build success
nor the controller tests prove modem or power timing on hardware.

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

The ESP-Mosaico profile proves ESP32-S31 target selection, WAMR/AOT
configuration, 16 KiB MMU-page-safe BundleFS, native Wi-Fi, CO5300 display,
the `78/esp_lcd_touch_cst92xx` interrupt-driven touch component, BQ27220
battery, ES8311/NS4150B audio and the
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
are present. The external 128 MiB SPI NAND (SPI3_HOST, single-line SPI at 40 MHz; WP#
and HOLD# are driven high as GPIO outputs in non-quad modes) is registered through
`BoardRegistration::SetAppStorage` as a `device::BlockStorage` with an explicit
4 KiB BundleFS block size (`kNandBundleBlockSize`, two FTL sectors) as a fixed
(non-removable) medium, so `FirmwareApp` may format it automatically when it holds no
BundleFS or another geometry; boards that expose a user-owned card must pass
`removable = true` so formatting only happens after confirmation in the System UI.
`FirmwareApp` mounts a second BundleFS on it for downloaded Apps. Module discovery
remains P2.

Every Board implementation submits one `BoardRegistration`. A hardware release
profile supplies both its human-readable `board` and stable `firmware_target`;
the latter selects OTA artifacts because several boards can share one chip.
Add Graphics/Input/System UI when
the display path is ready, then add audio, battery, Wi-Fi, power and external
devices independently. Audio silicon implements `AudioOutputPeripheral`; the shared
`AudioEngine` owns mixing and public Audio behavior. A board with accelerated
Guest↔Hall presentation implements the complete `DisplayTransition` interface;
a board without it returns no transition interface.

Register-controlled I²S codecs reuse `platform/audio/I2sCodecAudioSink` for
bus serialization, DMA, sample conversion and lifecycle. Codec-specific
classes such as ES8311 and AW88298 only construct their own `audio_codec_if_t`;
their register configuration must not be mixed into another codec's class.

Sensors, GPIO and haptics are Peripherals with board-local Channel values. The
board never assigns a public `DeviceId`, wire kind or capability bits. It calls
`AddSensor`, `AddGpio` or `AddHaptics` with its Peripheral, local Channel and a
physical display name; `DeviceRegistry` owns enumeration and public IDs. GPIO
names may follow the hardware manual (`P15`, `GPIO15`, `IO15`, and so on).
Names are descriptive only: Peripheral routing uses the local Channel and Guest
routing uses the upper-assigned opaque ID.

Vector sensors share `platform/sensors/PolledVectorSensorPeripheral`. Each board
owns a fixed array of channels (local ID, sensor kind, driver and timer name);
the sampler borrows that storage and owns timer, queue and cache lifecycle.
Drivers and the I2C executor outlive the sampler. Initialization and axis mapping
remain board-specific; the inertial adapter initializes one shared IMU before
binding its two vector channels. Start, Stop and destruction run on the owning
task, outside the I2C worker, and drain queued sampling before reconfiguration or
storage release. Read only copies the latest cache.

Claw4 composes its drivers, channel storage and sampler in Board state and initializes
them in the board startup path. BOX3 and CoreS3 compose their drivers and the shared
inertial adapter directly in Board. Board-specific sensor wrappers are reserved for
additional behavior, such as SZPI axis mapping or Mosaico asynchronous discovery;
simple interface forwarding does not need another class or source file.

Application GPIO uses `platform/gpio/EspGpioPeripheral`, configured with each
board's pin whitelist. Its control object remains in internal SRAM for ISR
access; board display and haptic PWM channels remain separate from the two
application PWM slots (LEDC timers/channels 2 and 3).

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
For the S31 target use `bash tools/s31.sh build-null` and
`bash tools/s31.sh build-host`. `s31-null` cannot be flashed; the physical
bring-up profile exposes `flash-host` and `monitor` with mandatory S31 chip
verification.
