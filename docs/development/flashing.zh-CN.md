# ESP32-P4 烧录指南

本文说明如何将 MicroPixel Host 和 App Store 烧录到 Metalio-Claw4（ESP32-P4）。以下命令均在
仓库根目录执行。

## 1. 准备环境

先按根目录 [README](../../README.md#环境) 安装 ESP-IDF 6.1、WASI Clang、`wamrc` 和 Python 依赖。
每个新终端都必须先激活 ESP-IDF；`export.sh` 会设置 `IDF_PATH` 并切换到匹配的 Python 环境：

```sh
source /path/to/esp-idf/export.sh
export WASI_SDK_PATH=/path/to/wasi-sdk
export WAMRC=/path/to/wamrc
python3 -m pip install -r requirements-dev.txt
```

确认 ESP-IDF 主版本为 6.1：

```sh
idf.py --version
```

## 2. 识别目标设备

使用可传输数据的 USB 线连接开发板。macOS 上先列出 USB 串口：

```sh
ls /dev/cu.usbmodem*
```

选择候选端口，再读取芯片型号和 MAC：

```sh
export P4_PORT=/dev/cu.usbmodemXXXX
python -m esptool --port "$P4_PORT" chip-id
```

只有输出 `ESP32-P4` 的端口才可用于烧录。同时连接多台设备时，必须在每次烧录前记录 MAC，
不能只根据 `/dev/cu.usbmodemXXXX` 的名称判断设备。USB 重新枚举后，同一个端口名可能指向另一台设备。

如果同一块板出现多个 CDC 端口，以 `esptool` 能识别为 ESP32-P4 的 USB Serial/JTAG 端口
为准。所有端口都无法连接时，检查数据线和供电，并按开发板的操作说明进入下载模式。

## 3. 完整烧录 System Shell

新设备或需要同时更新 Host 与全部内置 App 时，使用：

```sh
bash tools/flash_system_shell_p4.sh "$P4_PORT"
```

该入口会：

1. 运行 Host 测试、架构检查和音频门禁；
2. 构建 Host 固件以及 Blocks、Snake、Demo Bundle；
3. 生成包含三个 App 的 App Store 镜像；
4. 烧录 bootloader、分区表、OTA 初始数据和 Host 固件；
5. 烧录 App Store，并读回校验其 digest。

成功时命令末尾会输出：

```text
System Shell P4 flashed and verified ... with Blocks, Snake and Demo.
```

设备复位后应停留在 App Hall。

## 4. 只更新 App Store

Host 固件和分区表未变化时，可只更新已构建的 Guest Bundle：

```sh
bash tools/build_blocks_bundle.sh
bash tools/build_snake_bundle.sh
bash tools/build_demo_bundle.sh

bash tools/flash_guest_p4.sh "$P4_PORT" \
  build/apps/blocks/blocks.bundle.bin \
  build/apps/snake/snake.bundle.bin \
  build/apps/demo/demo.bundle.bin
```

`flash_guest_p4.sh` 会根据 Host 构建产物中的分区表解析 `app_store` 偏移和容量，不会覆盖
Host 固件。如果 Host ABI、Bundle 格式或分区表已变化，必须使用第 3 节的完整烧录。

## 5. Baseline 与串口调试

只烧录 Host 和单个 App Bundle 的基础入口为：

```sh
bash tools/build_p4_baseline.sh
bash tools/flash_p4_baseline.sh "$P4_PORT"
```

构建、烧录 Hello Guest 并等待成功标记：

```sh
bash tools/run_p4_baseline.sh "$P4_PORT"
```

查看持续串口日志：

```sh
bash tools/monitor_p4.sh "$P4_PORT"
```

监视器占用串口时，其他烧录或抓取命令无法同时使用该端口。退出监视器后再烧录。

## 6. 验收与常见问题

完整烧录后至少检查：

- 终端中 Host 固件和 App Store 均报告写入校验成功；
- 设备重启后显示 App Hall；
- App Hall 中可见 Blocks、Snake 和 Demo；
- 三个 App 都能启动，且状态栏中的亮度和音量控制生效。

常见失败：

- `IDF_PATH is not set`：当前终端尚未 `source` ESP-IDF 6.1 的 `export.sh`；
- `Serial port not found`：设备重新枚举后端口名已变，重新执行 `ls /dev/cu.usbmodem*`；
- `No serial data received`：选错 CDC 端口、USB 线不支持数据，或设备未进入可下载状态；
- 多台设备串口名重用：再次运行 `esptool chip-id`，以 MAC 而不是端口名确认目标。
