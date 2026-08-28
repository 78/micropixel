# Host 固件构建与烧录指南

本文说明如何构建和烧录 Metalio-Claw4（ESP32-P4）产品固件，以及如何构建和烧录
ESP-Mosaico（ESP32-S31）bring-up 固件。以下命令均在仓库根目录执行。

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

底层 build/flash/monitor 参数由 `tools/firmware.py` 和 `tools/firmware_profiles.json` 统一管理。日常仍使用
每块板的短 shell 入口；查看当前 profile 及其能力：

```sh
python3 tools/firmware.py list
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

`build-host` 不构建 Guest、不运行 unittest，也不执行 `fullclean`。`flash-host` 只烧录已经构建好的 Host
固件，不触发 Host 或 Guest 构建，也不写 `app_store`，所以保留已安装 App。`monitor` 使用已有 Host ELF 和固件 console
波特率，不触发构建、烧录、擦除或测试。只有确实需要丢弃 Host 构建缓存时才运行：

```sh
bash tools/p4.sh fullclean-host
```

## ESP32-S31 / ESP-Mosaico bring-up

ESP-Mosaico 使用 ESP32-S31 preview target 和独立 build 目录：

```sh
bash tools/s31.sh build-host
bash tools/s31.sh flash-host /dev/cu.usbmodemXXXX
bash tools/s31.sh monitor /dev/cu.usbmodemXXXX
bash tools/s31.sh monitor /dev/cu.usbmodemXXXX --reset  # 从应用启动开始抓日志
python3 tools/capture_screen.py /dev/cu.usbmodemXXXX logical.jpg --source logical --expect-size 480x480
python3 tools/capture_screen.py /dev/cu.usbmodemXXXX display.jpg --source display --expect-size 480x480
bash tools/s31.sh build-null
bash tools/s31.sh fullclean-mosaico  # 仅在需要重建 S31 配置时使用
```

只有一台匹配的 S31 时可以省略端口，也可用 `S31_PORT` 指定；默认 `S31_BAUD=460800`。`s31-null` 是依赖
方向编译门禁，没有 flash 或 monitor 能力。

ESP-Mosaico 板载 Type-C 连接的是 USB 2.0 HS OTG，而不是左侧模块接口引出的 USB Serial/JTAG。Host
固件在平台启动时初始化 TinyUSB CDC 控制台，因此应用启动后 Type-C 会重新枚举并可通过同一接口查看日志；
左侧 USB Serial/JTAG 仍保留为次输出。ROM 下载态与应用 TinyUSB 态使用不同 USB 描述符，macOS/Linux
设备节点可能随重枚举变化，不要把 `/dev/cu.usbmodem*` 或 `/dev/ttyACM*` 名称写死在自动化中。

MicroPixel 应用 CDC 实现 BSP 的 DTR/RTS 自动下载握手。`flash-host` 识别应用 USB 产品名后请求芯片进入
ROM，随后按同一 USB 物理位置等待 ROM 产品名并在同一个 esptool 连接中完成芯片校验和写入。这里刻意不在
写入前单独运行 `chip-id`：rev0 S31 的一次性 ROM 下载状态会被探测连接关闭时的控制线变化消耗掉。该闭环已
在 macOS 真机验证；首次烧录、应用固件损坏或应用 CDC 未启动时，仍需按板卡说明手动进入 ROM 下载模式。

当前 `esp-mosaico` 是 P0/P1 bring-up profile：CO5300 显示、CST9217 中断触摸、板级 3V3 电源、共用 Runtime、
BundleFS、native Wi-Fi、共享 App Hall/Status Layer 和 PPA/DMA2D 转场已接入；RGB565/QSPI 只作为板级
presentation boundary，正常刷新和转场不使用 CPU 整图逐像素换序。音频、传感器、NAND 与模块发现仍未
纳入当前范围。因此能烧录和显示大厅不代表产品功能已经完整。
如果 ESP-IDF preview 自身出现源码/header 不同步，应更新或重装对应 SDK，不在项目仓库中修补本机 IDF。

Host 与 Guest App 是两个独立更新通道。只修改固件时执行上面的 `build-host` + `flash-host`；只修改 SDK Demo
时使用产品固件的 USB 本地控制通道增量安装，不进入 ROM 下载态，也不重写 Host：

```sh
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX \
    app install guest/apps/demo
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

## 6. USB 增量安装与 App 控制

产品固件正常运行时，优先使用本地控制协议安装单个 Bundle。该路径调用 Host 的 App Store 安装事务，
不会擦除或重写整个 `app_store`：

```sh
python3 tools/micropixel --transport usb --port "$P4_PORT" app list
python3 tools/micropixel --transport usb --port "$P4_PORT" app install build/apps/demo/demo.bundle.bin
python3 tools/micropixel --transport usb --port "$P4_PORT" app start ai.micropixel.demo
python3 tools/micropixel --transport usb --port "$P4_PORT" app stop
python3 tools/micropixel --transport usb --port "$P4_PORT" app uninstall ai.micropixel.demo
```

`micropixel --transport usb app install guest/apps/demo` 会先按正式流程构建和打包，再通过同一协议安装；
传入现有 `.bundle.bin` 时则直接安装。安装、
升级和卸载仍由 HostController 串行执行；运行中的 Guest 必须先停止。Bundle 数据使用分块确认传输，
设备重新计算 SHA-256，并在完整 Bundle/AOT/资源校验通过后提交 BundleFS Catalog。

macOS/Linux 端口通常形如 `/dev/cu.usbmodemXXXX` 或 `/dev/ttyACM0`；Windows 端口形如 `COM7`，可直接传给
`--port`。端口自动探测同样按 ESP32 USB Serial/JTAG 的 VID/PID 工作。当前实现已在 macOS 真机验证；
Windows 使用 pyserial 的 COM 端口后端，代码路径受支持，但尚未完成项目真机验证。

该协议依赖正在运行的 Host 固件，不适用于下载模式或 bootloader。monitor、esptool、截图脚本和本地控制
共享板卡的 USB CDC 端口，不能同时占用。`capture_screen.py` 在 P4 与 S31 上使用相同 JPEG framing；S31
可分别抓取 LVGL 逻辑场景和当前显示提交缓冲，以定位面板传输类问题。

## 7. Conformance 配置与串口调试

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
