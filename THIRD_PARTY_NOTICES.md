# Third-party notices

Unless otherwise noted, project-authored source code, documentation, and assets are licensed under the
[Apache License 2.0](LICENSE).

## WebAssembly Micro Runtime

`firmware/espressif/components/wasm-micro-runtime/` is a Git submodule of the MicroPixel WebAssembly Micro Runtime
(WAMR) fork:

- fork: <https://github.com/78/wasm-micro-runtime>;
- branch: `wamr-host/esp-idf-psram`;
- pinned commit: `482b17e07fc46e80ffd23e5290871d42c49748e7`;
- upstream: <https://github.com/bytecodealliance/wasm-micro-runtime>;
- license: Apache-2.0 WITH LLVM-exception.

After submodule initialization, the applicable license, third-party attributions, and component-specific license
files are present in that directory.

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
vendored in this repository. Exact resolved versions are recorded in `dependencies.lock` files. Their own license
terms apply when they are downloaded or redistributed in a binary release.

### esp_codec_dev

ESP-Mosaico ES8311 initialization uses `espressif/esp_codec_dev` 1.6.2 from the ESP Component Registry:

- upstream: <https://components.espressif.com/components/espressif/esp_codec_dev>, Apache-2.0;
- scope: ES8311 codec configuration and the ESP-IDF I2S data interface; MicroPixel supplies the shared-I2C-executor
  control interface and keeps Host master-volume attenuation in its fixed-capacity audio backend.

The exact resolved version is recorded in the ESP32-S31 dependency lock, and the downloaded component retains its
complete license file.

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

The exact resolved versions are recorded in the ESP32-S31 dependency lock, and the downloaded components retain
their complete license files.

## Font Awesome Wi-Fi glyph

`firmware/espressif/main/platform/lvgl/ui/square_720/icons/wifi_status_icons.c` contains fixed-size alpha masks derived
from the Font Awesome 5 Wi-Fi glyph bundled with LVGL. The weak and medium variants preserve the source glyph's
baseline and footprint:

- upstream: <https://fontawesome.com>;
- copyright: Copyright 2022 Fonticons, Inc.;
- font license: SIL Open Font License 1.1.

The alpha masks are generated by `tools/generate_wifi_status_icons.py` from LVGL's managed-component font source.
