# Third-party notices

Unless otherwise noted, project-authored source code, documentation, and assets are licensed under the
[Apache License 2.0](LICENSE).

## WebAssembly Micro Runtime

`firmware/espressif/components/wasm-micro-runtime/` is a Git submodule of the MicroPixel WebAssembly Micro Runtime
(WAMR) fork:

- fork: <https://github.com/78/wasm-micro-runtime>;
- branch: `wamr-host/esp-idf-psram`;
- pinned commit: `4dbe3b6efe776fde06468e47f342c1d351879cf0`;
- upstream: <https://github.com/bytecodealliance/wasm-micro-runtime>;
- license: Apache-2.0 WITH LLVM-exception.

After submodule initialization, the applicable license, third-party attributions, and component-specific license
files are present in that directory.

## ESP-IoT-Solution

`firmware/espressif/components/esp-iot-solution/` is a Git submodule of the MicroPixel fork of Espressif's
esp-iot-solution; the build only uses its `esp_lvgl_adapter` component through `override_path`:

- fork: <https://github.com/78/esp-iot-solution>;
- branch: `codex/lvgl-monotonic-tick`;
- pinned commit: `bdff5cab67300f7fe705ed47af2c4c0ace840a2c`;
- upstream: <https://github.com/espressif/esp-iot-solution>;
- copyright: Espressif Systems (Shanghai) CO LTD;
- license: Apache-2.0.

## LLVM libc++ in Guest applications

MicroPixel Guest applications are compiled with libc++ headers and selected static-library objects distributed by
wasi-sdk. Link-time garbage collection retains only objects referenced by each Guest:

- upstream: <https://github.com/llvm/llvm-project/tree/main/libcxx>;
- toolchain distribution: <https://github.com/WebAssembly/wasi-sdk>;
- license: Apache-2.0 WITH LLVM-exception.

The toolchain distributions contain the complete applicable license and attribution files. MicroPixel does not
copy the libc++ source tree into this repository.

## MetalioClaw4 display driver

Portions of `firmware/espressif/main/platform/drivers/display/nv3051f/esp_lcd_nv3051f.c` and
`firmware/espressif/main/platform/drivers/display/nv3051f/esp_lcd_nv3051f.h`, including the panel initialization sequence, are derived
from the MetalioClaw4 project:

- upstream: <https://github.com/CloudZao/MetalioClaw4>;
- referenced revision: `5a9841fd2cbd`;
- license: MIT.

The upstream MIT notice follows:

> Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
>
> Copyright (c) 2025 Project Contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
> TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

## ESP-IDF managed components

ESP-IDF, LVGL, and components resolved from the Espressif Component Registry are build dependencies and are not
vendored in this repository. Version constraints are declared in `firmware/espressif/main/idf_component.yml`;
ESP-IDF-generated target lock files are local build outputs and are not version-controlled. Each component's own
license terms apply when it is downloaded or redistributed in a binary release.

## Espressif flasher stub

The browser firmware installer loads the ESP32-S31 RAM flasher binary from the `esp-flasher-stub` npm package:

- upstream: <https://github.com/espressif/esp-flasher-stub>;
- package/version: `esp-flasher-stub` 1.2.2;
- license: Apache-2.0 OR MIT.

The package retains the complete Apache-2.0 and MIT license texts. MicroPixel uses the unmodified ESP32-S31 JSON
artifact and implements only the Web Serial upload and protocol integration around it.

## Bosch SensorAPI

`firmware/espressif/components/bosch_sensorapi/` contains the minimal unmodified C sources and headers used for
ESP-Mosaico's BMI270 inertial sensor and two BMM150 magnetometers:

- BMI270 SensorAPI: <https://github.com/boschsensortec/BMI270_SensorAPI>, pinned commit
  `41129fcfe39c583ee5462d79195741945d51c1fe`;
- BMM150 SensorAPI: <https://github.com/boschsensortec/BMM150_SensorAPI>, pinned commit
  `0dce0617873cda1f6d51f6b7b961fdc2641e0c7c`;
- copyright: Bosch Sensortec GmbH;
- license: BSD-3-Clause.

The complete upstream license text is retained alongside each copied driver. MicroPixel supplies only the
ESP-IDF transport callbacks and board-level scheduling wrappers.

### esp_codec_dev

ESP-Mosaico and ESP32-S3 ES8311/AW88298 initialization use `espressif/esp_codec_dev` 1.6.2 from the ESP Component Registry:

- upstream: <https://components.espressif.com/components/espressif/esp_codec_dev>, Apache-2.0;
- scope: ES8311 codec configuration and the ESP-IDF I2S data interface; MicroPixel supplies the shared-I2C-executor
  control interface and keeps Host master-volume attenuation in its fixed-capacity audio backend.

The declared version constraint is recorded in `firmware/espressif/main/idf_component.yml`, and the downloaded
component retains its complete license file.

### ESP-BOX-3 board definitions

The native ESP32-S3-BOX-3 backend derives its pin assignments, controller selection and vendor display initialization
sequence from Espressif's `esp-box-3` BSP 3.2.0:

- upstream: <https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3>, Apache-2.0;
- scope: only the board-specific I2C, display, touch and backlight facts needed by MicroPixel are maintained locally;
  the upstream BSP component is not linked, so it does not constrain the shared codec or LVGL dependency versions.

### M5Stack CoreS3 board definitions

The M5Stack CoreS3 backend derives its pin assignments, controller selection and power sequencing from Espressif's
`m5stack_core_s3` BSP:

- upstream: <https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3>, Apache-2.0;
- scope: board-specific I2C, display, touch, AW88298 audio and AXP2101/AW9523 power-control facts are maintained
  locally; the upstream BSP component is not linked.

### micro-opus and libopus

Ogg Opus playback uses `esphome/micro-opus` 0.4.1 from the ESP Component Registry:

- wrapper and Ogg demuxer: <https://github.com/esphome-libs/micro-opus>, Apache-2.0;
- bundled Xiph.Org libopus: <https://github.com/xiph/opus>, BSD 3-Clause;
- resolved component source revision: `8354085908683c6130e32a832aeec8a7ca115c51`.

The downloaded managed component contains the complete Apache-2.0 notice and the upstream libopus copyright and
license files. MicroPixel uses its streaming Ogg Opus decoder with one shared PSRAM-backed pseudostack and does not
expose the component API to Guest applications.

### esp_tinyusb and TinyUSB

ESP-Mosaico Type-C CDC uses `espressif/esp_tinyusb` 2.2.1 and its `espressif/tinyusb` dependency from the ESP
Component Registry:

- Espressif integration: <https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb>, Apache-2.0;
- TinyUSB upstream: <https://github.com/hathach/tinyusb>, MIT.

The declared version constraints are recorded in `firmware/espressif/main/idf_component.yml`, and the downloaded
components retain their complete license files.

## Font Awesome glyphs

MicroPixel's generated `builtin-latin-v1` semantic fonts include the public SDK symbol set from the Font Awesome 5
font bundled with LVGL. `firmware/espressif/main/platform/lvgl/ui/square_720/icons/wifi_status_icons.c` additionally
contains fixed-size alpha masks derived from its Wi-Fi glyph. The weak and medium Wi-Fi variants preserve the source
glyph's baseline and footprint:

- upstream: <https://fontawesome.com>;
- copyright: Copyright 2022 Fonticons, Inc.;
- font license: SIL Open Font License 1.1.

The alpha masks are generated by `tools/generate_wifi_status_icons.py` from LVGL's managed-component font source.
