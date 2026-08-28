# MicroPixel board port checklist

`boards/<board>/` is a Platform composition layer. A new board owns only its
pin mapping, peripheral composition, power sequencing, board metadata and any
device implementation that cannot be reused.

## Required files and registration

1. Add `boards/<board>/CMakeLists.txt` and the implementation that provides the
   single `micropixel::platform::ConfiguredPlatform()` symbol.
2. Add one `MICROPIXEL_BOARD_<NAME>` entry to the `MICROPIXEL_BOARD` choice in
   `main/Kconfig.projbuild`.
3. Select the board CMake file in `platform/CMakeLists.txt`; include only the
   `common/`, `drivers/` and `lvgl/` layers that the board uses.
4. Add a product `sdkconfig` defaults file. Keep compile-only profiles in a
   separate build directory and never expose a flash command for them.
5. Add one declarative entry to `tools/firmware_profiles.json`. Board shell
   scripts are aliases for product workflows; they must delegate ESP-IDF
   build, flash, monitor and chip-safe port selection to `tools/firmware.py`.

The ESP-Mosaico P0/P1 profile proves ESP32-S31 target selection, WAMR/AOT
configuration, 16 KiB MMU-page-safe BundleFS, native Wi-Fi, CO5300 display and
CST9217 interrupt-driven touch composition. It reuses the shared App Hall,
Status Layer, viewport/layout profiles, transition timeline and PPA/DMA2D
primitives; the board layer owns only the RGB565/QSPI presentation boundary.
RGB565 scaling/composition remains in canonical byte order and panel byte
packing is a separate 1:1 PPA pass, never a CPU full-frame pixel loop. ES8311,
BMI270/BMM150 and SAM8108 capabilities remain explicitly unavailable until
their authoritative BSP paths are integrated and validated. Do not copy
Metalio-Claw4 sequences or guess controller initialization. NAND and module
discovery are P2.

Every Platform implementation must provide all `Platform` accessors, including
`device::HardwareInfoBackend`. Unsupported capabilities return an unavailable
backend; they do not change Guest ABI or make Runtime include the board.

Before adding board-local code, check these homes:

- reusable ESP32 transport/backend/adapter: `platform/common/`;
- controller or peripheral driver: `platform/drivers/<kind>/<chip>/`;
- reusable display engine or resolution-specific Host UI:
  `platform/lvgl/`;
- hardware-independent contract: `device/`.

Run at least the Host regression suite, architecture check, format check and
the new board's full ESP-IDF build. Keep `bash tools/p4.sh build-null` passing.
For the preview S31 target use `bash tools/s31.sh build-null` and
`bash tools/s31.sh build-mosaico`. `s31-null` cannot be flashed; the physical
bring-up profile exposes `flash-mosaico` and `monitor` with mandatory S31 chip
verification.
