# MicroPixel

[English](README.md) | [简体中文](README.zh-CN.md)

[MicroPixel](https://micropixel.ai) runs WebAssembly apps on Espressif microcontrollers.
Apps use a C++23 SDK for graphics, input, audio, storage, and sensors, without depending on a board-specific SDK.
The firmware manages hardware, app isolation, and the system UI.

## Hardware

| Chip | Board |
|---|---|
| ESP32-P4 | [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4) |
| ESP32-S31 | [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) |
| ESP32-S3 | [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) |
| ESP32-S3 | [LCKFB SZPI](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html) |
| ESP32-S3 | [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) |

The Host uses ESP-IDF 6.1 and a pinned [WAMR fork](https://github.com/78/wasm-micro-runtime).
Apps are compiled to architecture-specific AOT v6 bundles. The ABI is still evolving.

## Build an app

Install the [SDK](https://micropixel.ai/docs/environment/) and connect a device running MicroPixel firmware.
App development does not require ESP-IDF.

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
micropixel --transport usb run
```

`run` builds, installs, starts the app, and follows its logs. Ctrl-C stops log streaming; the app keeps running.
See the [quickstart](guest/sdk/QUICKSTART.md) and [publishing guide](guest/sdk/PUBLISHING.md).

## Build the firmware

Initialize submodules, activate ESP-IDF 6.1, and configure WASI SDK and the matching MicroPixel WAMRC.
See the [build guide](docs/development/flashing.zh-CN.md) (Chinese) for toolchain setup and flashing.

```sh
git submodule update --init --recursive
python3 -m pip install -r requirements-dev.txt
bash tools/p4.sh build-host
```

Other profiles: `bash tools/s31.sh build-host` and `bash tools/s3.sh build-host <box3|szpi|cores3>`.

## Project

- [Guest SDK](guest/sdk/README.md) — app APIs and examples.
- [Firmware](firmware/README.md) — Host runtime and board support.
- [Documentation](docs/README.md) — architecture, protocols, and development guides.
- [Contributing](CONTRIBUTING.md).

## License

Project-authored code, documentation, and assets use [Apache-2.0](LICENSE), unless stated otherwise.
See [third-party notices](THIRD_PARTY_NOTICES.md) for dependency licenses and exceptions.
