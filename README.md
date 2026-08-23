# MicroPixel

[MicroPixel](https://micropixel.ai) 是一个面向嵌入式设备的 WebAssembly 应用运行时：Guest 应用通过稳定的 Host Service ABI
访问图形、输入、音频、存储和资源服务，不直接依赖芯片 SDK 或开发板类型。

当前支持范围是 ESP32-P4 + [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4)。Host 使用
ESP-IDF 6.1 和固定 commit 的 [WAMR fork](https://github.com/78/wasm-micro-runtime)，Guest 使用受限的
C++23 API，并由 WAMR 2.4.5 `wamrc` 编译为 RISC-V 32-bit AOT。项目正式名称为 MicroPixel，ABI
目前仍在演进中。

## 目录

```text
.
├── firmware/espressif/      # ESP32-P4 Host 固件与 WAMR fork submodule
├── guest/                   # ABI、Runtime、C++ SDK、应用和 conformance tests
├── docs/                    # 架构、开发规范、路线图和硬件来源链接
└── tools/                   # 构建、打包、烧录、串口和静态检查脚本
```

`build/`、`artifacts/`、ESP-IDF 的 `managed_components/` 和生成的 `sdkconfig` 都是本地输出，
不会进入版本控制。历史性能实验、真机日志和一次性测试数据也不作为项目源码发布。

克隆后先初始化 WAMR submodule：

```sh
git submodule update --init --recursive
```

## 环境

需要：

- ESP-IDF 6.1，并已通过其 `export.sh` 设置 `IDF_PATH`；本次清理验证使用 commit
  `6a9c44fe7e725af45cb99293ae38afd7d481f1e3`；
- 带 wasm32 backend 的 Clang（设置 `WASI_CLANG`，或设置 `WASI_SDK_PATH`）；
- WAMR 2.4.5 的 `wamrc`，目标为 `RISCV32_ILP32F`（设置 `WAMRC`）；
- Python 3；烧录和串口工具的 Python 依赖见 `requirements-dev.txt`。

```sh
source /path/to/esp-idf/export.sh
export WASI_SDK_PATH=/path/to/wasi-sdk
export WAMRC=/path/to/wamrc
python3 -m pip install -r requirements-dev.txt
```

## 构建与测试

```sh
# 构建 Host，以及当前仍维护的 ABI/SDK conformance Guests
bash tools/build_p4_baseline.sh

# 分别构建两个示例 App Bundle
bash tools/build_demo_bundle.sh
bash tools/build_snake_bundle.sh

# 只检查 Firmware 格式；完整 clang-tidy 首次需要 --configure
bash tools/check_firmware_style.sh --format-only
```

`guest/tests/conformance/` 是仍在构建链路中的回归用例，应予保留。需要合成 Host 事件的两项板端用例
使用 `firmware/espressif/sdkconfig.p4-conformance.defaults`；产品 defaults 默认关闭这些 test hooks。
用于调试 App 的 USB 屏幕抓取也保留在独立的、默认关闭的
`firmware/espressif/sdkconfig.p4-screen-capture.defaults` overlay 中。
构建、烧录和监视的详细入口见 [Firmware README](firmware/espressif/README.md)，Guest API 与 Bundle
入口见 [Guest README](guest/README.md)。

## 设计文档

- [应用框架 v0.4 讨论稿](docs/design/application-framework-v0.4.zh-CN.md)
- [Firmware 架构](docs/design/firmware-architecture.zh-CN.md)
- [Guest–Host Service ABI](docs/design/guest-host-service-abi.zh-CN.md)
- [C/C++ 代码风格](docs/development/code-style.zh-CN.md)

设计稿用于记录当前方向，不代表接口已经冻结。硬件资料只保留官方来源链接，见
[Hardware references](docs/hardware/README.md)。

## 许可证

项目自有代码、文档和素材默认采用 [Apache License 2.0](LICENSE)。WAMR submodule、NV3051F 驱动和
其他依赖的归属与例外见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

贡献前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)；安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。
