# Espressif firmware

ESP-IDF 6.1 Host firmware with a pinned MicroPixel WAMR fork. Run commands from the repository root
with the [toolchain configured](../docs/development/flashing.zh-CN.md) (Chinese).

```sh
bash tools/p4.sh build-host
bash tools/s31.sh build-host
bash tools/s3.sh build-host <box3|szpi|cores3>
```

Build and flash are separate operations. For example, `bash tools/p4.sh flash-host <port>` writes the built Host
and preserves the App Store. Stop other serial tools before connecting.

- [Build, configuration, and flashing (中文)](../docs/development/flashing.zh-CN.md)
- [Source guide](espressif/main/README.zh-CN.md)
- [Contributing](../CONTRIBUTING.md)
- [简体中文](README.zh-CN.md)
