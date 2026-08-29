# MicroPixel

[MicroPixel](https://micropixel.ai) 是一个面向嵌入式设备的 WebAssembly 应用运行时：Guest 应用通过稳定的 Host Service ABI
访问图形、输入、音频、存储和资源服务，不直接依赖芯片 SDK 或开发板类型。

当前产品固件运行在 ESP32-P4 + [Metalio-Claw4](https://github.com/CloudZao/MetalioClaw4)；
ESP32-S31 + ESP-Mosaico P0/P1 作为 preview bring-up profile 同步维护。Host 使用 ESP-IDF 6.1 和固定
commit 的 [WAMR fork](https://github.com/78/wasm-micro-runtime)，Guest 使用受限 C++23 API，并由同一
fork commit 的 `wamrc` 编译为 RISC-V 32-bit AOT format v6。项目正式名称为 MicroPixel，ABI 目前仍在演进中。

## 目录

```text
.
├── firmware/espressif/      # ESP32-P4 产品、ESP32-S31 preview 与 WAMR fork submodule
├── guest/                   # ABI、Runtime、C++ SDK、应用和 conformance tests
├── docs/                    # 当前架构、开发规范和硬件来源
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

- ESP-IDF 6.1，并已通过其 `export.sh` 设置 `IDF_PATH`；当前验证基线使用 commit
  `6a9c44fe7e725af45cb99293ae38afd7d481f1e3`；
- 带 wasm32 backend 的 Clang（设置 `WASI_CLANG`，或设置 `WASI_SDK_PATH`）；
- MicroPixel WAMR fork 固定 commit `482b17e07fc46e80ffd23e5290871d42c49748e7` 构建的 `wamrc`，目标为
  `RISCV32_ILP32F`、AOT format v6（设置 `WAMRC`）；上游 WAMR 2.4.3 至 2.4.5 的 AOT v5 不兼容；
- Python 3；烧录和串口工具的 Python 依赖见 `requirements-dev.txt`。

```sh
source /path/to/esp-idf/export.sh
export WASI_SDK_PATH=/path/to/wasi-sdk
export WAMRC=/path/to/wamrc
python3 -m pip install -r requirements-dev.txt
```

本机默认的 `IDF_PATH`、`P4_PORT`/`P4_BAUD`、`S31_PORT`/`S31_BAUD` 和
`MICROPIXEL_REMOTE_CONTROL_HOST` 可写在被 Git 忽略的根目录
`.env` 中；`tools/p4.sh` 与 `tools/s31.sh` 会自动加载，并将远控地址写入生成的
`CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST`。显式环境变量或命令行端口优先级更高。

Host 的 ESP-IDF 命令统一由 `tools/firmware.py` 读取 `tools/firmware_profiles.json` 生成；`p4.sh` 和
`s31.sh` 只保留板型易记命令及各自的产品流程。可用 profile 可直接查看：

```sh
python3 tools/firmware.py list
```

新增开发板时应增加声明式 profile，而不是复制一份 ESP-IDF build/flash/monitor shell 实现。Null
profile 只有编译能力，通用工具会拒绝烧录；物理板 profile 在烧录前即使传入了显式端口，也会通过
`esptool chip-id` 核对实际芯片，避免把固件写到错误目标。

`p4.sh` 会在自身进程内退出已激活的 Conda 环境，避免 Conda Python/动态库污染 ESP-IDF venv；它不会也
无法改变父终端的 Conda 状态。ESP-IDF 官方导出的工具环境缓存在 `build/p4-idf-environment.sh`，仅首次运行
或 IDF/脚本更新后重新生成，因此无需每次手动执行 `export.sh`。

## 构建与测试

单个 Guest App 由统一 CLI 直接读取 `app.json`，不需要 App 专用构建脚本。日常设备开发使用一条 `run`：

```sh
python3 tools/micropixel --transport usb run guest/apps/demo
```

安装后的 CLI 可在项目目录直接运行 `micropixel --transport usb run`，默认读取当前目录的 `app.json`。
`micropixel run` 会以 development profile
构建，停止当前 Guest，安装并启动目标 App，然后持续输出日志；按 `Ctrl-C` 只退出日志跟随，App 继续运行。
只需本地 AOT 或正式 Bundle 时，再单独使用项目级 `micropixel build` 或 `micropixel package`。
仓库级 Host 与集成构建仍使用：

```sh
# 日常增量构建 Host；不构建 Guest、不跑 unittest、不执行 fullclean
bash tools/p4.sh build-host

# 构建 Host、7 个示例 App 和 App Store 镜像；不跑测试
bash tools/p4.sh build-all

# 仅在发布前或推送前显式运行完整测试门禁
bash tools/p4.sh test
```

ESP-Mosaico 的 ESP32-S31 P0/P1 bring-up 使用独立入口。`build-host` 与 `flash-host` 明确分离，后者只写
已经构建好的 Host，不重建或改写 SDK Demo/App Store。首次 ROM 烧录后，MicroPixel 应用 CDC 会自动切换到下载模式，脚本按 USB 物理位置接续
重枚举后的 ROM 端口：

```sh
bash tools/s31.sh build-host
bash tools/s31.sh flash-host /dev/cu.usbmodemXXXX
bash tools/s31.sh monitor /dev/cu.usbmodemXXXX
bash tools/s31.sh build-null
```

SDK Demo 单独通过 USB 增量安装，不烧录 Host：

```sh
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX \
    app install guest/apps/demo
```

设备连接后，日常 Host 修改使用保留 App Store 的增量烧录入口：

```sh
bash tools/p4.sh flash-host /dev/cu.usbmodemXXXX
bash tools/p4.sh monitor /dev/cu.usbmodemXXXX
```

两条命令在只连接一台 ESP32-P4 时都可省略端口；`monitor` 不构建、不烧录、不清空数据，也不运行测试。
USB 调试的 App 烧录默认清空旧 Catalog 并写入 7 个示例 App：

```sh
bash tools/p4.sh flash-apps
```

日常 App 开发可直接通过正在运行的产品固件增量安装，不写整个 `app_store` 分区，也不需要 Remote Control
Token：

```sh
python3 tools/micropixel --transport usb app list
python3 tools/micropixel --transport usb app install guest/apps/demo
python3 tools/micropixel --transport usb app start micropixel.demo
python3 tools/micropixel --transport usb app start micropixel.demo --follow
python3 tools/micropixel --transport usb app stop
python3 tools/micropixel --transport usb app uninstall micropixel.demo
```

在 App 项目目录中可以省略路径和 App ID，并把开发闭环合并成一条命令：

```sh
micropixel --transport usb run
```

不需要跟随日志时使用 `micropixel --transport usb run --no-follow`。构建或打包失败发生在设备操作之前；
安装或启动失败时，CLI 会尽力恢复此前运行的 App。

只有一台 ESP32 USB Serial/JTAG 设备时端口会自动探测；否则在 macOS/Linux 使用
`--port /dev/cu.usbmodemXXXX`，在 Windows 使用 `--port COM7`（替换为设备管理器显示的端口）。USB
本地控制与串口 monitor/esptool 共享端口，使用前应退出 monitor。当前已在 macOS 真机验证；Windows
由 pyserial COM 端口后端支持，但尚未完成项目真机验证。
协议和安全边界见 [USB 本地控制](docs/design/usb-local-control.zh-CN.md)。

`fullclean-host`、Host-only 烧录和完整 Host+Apps 烧录都是独立的显式命令；详见
[Host 构建与烧录指南](docs/development/flashing.zh-CN.md)。

`guest/tests/conformance/` 是仍在构建链路中的回归用例，应予保留。需要合成 Host 事件的板端用例
使用 `firmware/espressif/sdkconfig.p4-conformance.defaults`；产品 defaults 默认关闭这些 test hooks。
用于 App 开发调试的 USB 屏幕抓取与触摸注入通道直接包含在 P4 产品固件中，不增加 Guest ABI。
构建、烧录和监视的详细入口见 [Firmware README](firmware/espressif/README.md)，Guest API 与 Bundle
入口见 [Guest README](guest/README.md)。

## 文档

- [架构与发布基线](docs/design/architecture.zh-CN.md)
- [Guest C++ SDK](guest/sdk/README.md)
- [Guest–Host ABI](guest/abi/README.md)
- [C/C++ 代码风格](docs/development/code-style.zh-CN.md)
- [定时器与大厅空闲功耗](docs/development/timers-and-idle-power.zh-CN.md)
- [Host 构建与烧录指南](docs/development/flashing.zh-CN.md)

文档索引、维护规则和硬件官方来源见 [Documentation](docs/README.md)。

## 许可证

项目自有代码、文档和素材默认采用 [Apache License 2.0](LICENSE)。WAMR submodule、NV3051F 驱动和
其他依赖的归属与例外见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

贡献前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)；安全问题请按 [SECURITY.md](SECURITY.md) 私下报告。
