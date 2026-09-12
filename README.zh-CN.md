# MicroPixel

[English](README.md) | [简体中文](README.zh-CN.md)

[MicroPixel](https://micropixel.ai) 是 Espressif 微控制器上的 WebAssembly 应用运行时。
应用通过 C++23 SDK 使用图形、输入、音频、存储和传感器，无需依赖板级 SDK。
固件负责硬件管理、应用隔离和系统界面。

## 支持硬件

| 芯片 | 开发板 |
|---|---|
| ESP32-P4 | [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4) |
| ESP32-S31 | [ESP-Mosaico](https://github.com/esp-mosaico/esp-mosaico-bsp) |
| ESP32-S3 | [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) |
| ESP32-S3 | [立创 SZPI](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html) |
| ESP32-S3 | [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) |

Host 使用 ESP-IDF 6.1 和固定版本的 [WAMR fork](https://github.com/78/wasm-micro-runtime)。
应用按目标架构编译为 AOT v6 Bundle，ABI 仍在演进。

## 开发应用

安装 [SDK](https://micropixel.ai/docs/environment/)，连接已运行 MicroPixel 固件的设备。
应用开发不需要 ESP-IDF。

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
micropixel --transport usb run
```

`run` 构建、安装、启动应用并跟随日志。Ctrl-C 退出日志跟随，应用继续运行。
详见[快速入门](guest/sdk/QUICKSTART.zh-CN.md)与[发布指南](guest/sdk/PUBLISHING.zh-CN.md)。

## 构建固件

初始化子模块，激活 ESP-IDF 6.1，配置 WASI SDK 和匹配的 MicroPixel WAMRC。
工具链安装与烧录步骤见[构建指南](docs/development/flashing.zh-CN.md)。

```sh
git submodule update --init --recursive
python3 -m pip install -r requirements-dev.txt
bash tools/p4.sh build-host
```

其他板型：`bash tools/s31.sh build-host`、`bash tools/s3.sh build-host <box3|szpi|cores3>`。

## 项目文档

- [Guest SDK](guest/sdk/README.zh-CN.md)：应用接口与示例。
- [Firmware](firmware/README.zh-CN.md)：Host 运行时与板级支持。
- [文档索引](docs/README.zh-CN.md)：架构、协议与开发指南。
- [参与贡献](CONTRIBUTING.zh-CN.md)。

## 许可证

项目自有代码、文档和素材默认采用 [Apache-2.0](LICENSE)。
依赖许可证与例外见[第三方声明](THIRD_PARTY_NOTICES.md)。
