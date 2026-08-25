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

统一烧录入口默认使用 `P4_BAUD=2000000`；需要为特殊 USB 链路降速时，可在根目录 `.env` 或当前环境中
覆盖该值。

只有输出 `ESP32-P4` 的端口才可用于烧录。同时连接多台设备时，必须在每次烧录前记录 MAC，
不能只根据 `/dev/cu.usbmodemXXXX` 的名称判断设备。USB 重新枚举后，同一个端口名可能指向另一台设备。

如果同一块板出现多个 CDC 端口，以 `esptool` 能识别为 ESP32-P4 的 USB Serial/JTAG 端口
为准。所有端口都无法连接时，检查数据线和供电，并按开发板的操作说明进入下载模式。

## 3. 日常增量构建和烧录 Host

修改 Host C/C++ 后使用：

```sh
bash tools/p4.sh build-host
bash tools/p4.sh flash-host "$P4_PORT"  # 只有一台 P4 时可省略端口
bash tools/p4.sh monitor "$P4_PORT"     # 持续查看串口；Ctrl+] 退出
```

`build-host` 不构建 Guest、不运行 unittest，也不执行 `fullclean`。`flash-host` 先做相同的增量构建，
再烧录 Host 固件；它不写 `app_store`，所以保留已安装 App。`monitor` 使用已有 Host ELF 和固件 console
波特率，不触发构建、烧录、擦除或测试。只有确实需要丢弃 Host 构建缓存时才运行：

```sh
bash tools/p4.sh fullclean-host
```

## 4. 完整烧录 Host 和七个示例 App

新设备或需要同时更新 Host 与 BundleFS 元数据时，使用：

```sh
bash tools/p4.sh flash-all "$P4_PORT"
```

该入口会：

1. 构建 Host 固件和 Blocks、Snake、Demo 及四个 Showcase Bundle；
2. 生成包含七个 App 的 BundleFS 镜像；
3. 烧录 bootloader、分区表、OTA 初始数据和 Host 固件；
4. 清空并烧录 App Store，随后读回校验。

该命令不运行测试。发布前或推送前单独执行：

```sh
bash tools/p4.sh test
```

成功时命令末尾会输出：

```text
System Shell P4 flashed and verified ... with seven Apps.
```

设备复位后应在 App Hall 中看到七个 App，并可在第一行左右滑动浏览。

## 5. USB 烧录七个示例 App

Host 固件和分区表未变化时，直接清空旧 App Store 并烧录已有的七个示例 Bundle：

```sh
bash tools/p4.sh flash-apps "$P4_PORT"
```

只有一台 ESP32-P4 时可省略 `PORT`，脚本会自动探测。该命令不重新构建 Bundle；产物缺失时先运行
`bash tools/p4.sh build-apps`。它会替换 Catalog 和示例 App 数据，但不覆盖 Host 固件。

空 Catalog 只用于格式恢复，入口刻意命名为：

```sh
bash tools/p4.sh reset-app-store "$P4_PORT"
```

## 6. Conformance 配置与串口调试

需要专用 conformance Host 时，仍通过统一入口显式叠加配置：

```sh
P4_SDKCONFIG_DEFAULTS="$PWD/firmware/espressif/sdkconfig.p4.defaults;$PWD/firmware/espressif/sdkconfig.p4-conformance.defaults" \
    bash tools/p4.sh build-host
bash tools/p4.sh flash-host "$P4_PORT"
```

这不会隐式清空 App Store。只有测试明确要求空 BundleFS 时才执行：

```sh
bash tools/p4.sh reset-app-store "$P4_PORT"
```

查看持续串口日志：

```sh
bash tools/p4.sh monitor "$P4_PORT"
```

监视器占用串口时，其他烧录或抓取命令无法同时使用该端口。退出监视器后再烧录。

## 7. 验收与常见问题

完整烧录后至少检查：

- 终端中 Host 固件和 BundleFS 元数据均报告写入校验成功；
- 烧录脚本捕获到 `System Shell ready: App Hall rendered with apps=7`；
- App Hall 中可左右滑动浏览并启动七个示例 App；
- 状态栏中的亮度和音量控制生效。

常见失败：

- `IDF_PATH is not set`：当前终端尚未 `source` ESP-IDF 6.1 的 `export.sh`；
- `Serial port not found`：设备重新枚举后端口名已变，重新执行 `ls /dev/cu.usbmodem*`；
- `No serial data received`：选错 CDC 端口、USB 线不支持数据，或设备未进入可下载状态；
- 多台设备串口名重用：再次运行 `esptool chip-id`，以 MAC 而不是端口名确认目标。
