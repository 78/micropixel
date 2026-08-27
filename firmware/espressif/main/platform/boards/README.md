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
