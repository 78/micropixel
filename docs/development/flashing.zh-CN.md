# Host 固件构建与烧录指南

本文说明如何构建和烧录 Metalio-Claw4（ESP32-P4）产品固件，以及 ESP-Mosaico（ESP32-S31）、
ESP32-S3-BOX-3、立创开发板 SZPI ESP32-S3 和 M5Stack CoreS3 预览固件。以下命令均在仓库根目录执行。

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

## ESP32-S31 / ESP-Mosaico 预览版

ESP-Mosaico 使用 ESP32-S31 preview target 和独立 build 目录：

```sh
bash tools/s31.sh build-host
bash tools/s31.sh build-release  # 发布用：Host + Blocks/Snake/Tilt/Demo + 完整浏览器镜像
bash tools/s31.sh flash-host /dev/cu.usbmodemXXXX
bash tools/s31.sh monitor /dev/cu.usbmodemXXXX
bash tools/s31.sh monitor /dev/cu.usbmodemXXXX --reset  # 从应用启动开始抓日志
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX screenshot --output screenshot.jpg
bash tools/s31.sh build-null
bash tools/s31.sh fullclean-mosaico  # 仅在需要重建 S31 配置时使用
```

只有一台匹配的 S31 时可以省略端口，也可用 `S31_PORT` 指定；默认 `S31_BAUD=460800`。`s31-null` 是依赖
方向编译门禁，没有 flash 或 monitor 能力。

`build-release` 生成 `build/host-esp32s31-mosaico/micropixel-full.bin`，供在线烧录页使用。完整镜像中的
App Store 固定包含 Blocks、Snake、Tilt 和 SDK Demo 四个集成 App；生成器从 ESP-IDF 的
`flasher_args.json` 读取 S31 的 16 MiB Flash 容量并拒绝任何越界区域。

ESP-Mosaico 板载 Type-C 连接的是 USB 2.0 HS OTG，而不是左侧模块接口引出的 USB Serial/JTAG。Host
固件在平台启动时初始化 TinyUSB CDC 控制台，因此应用启动后 Type-C 会重新枚举并可通过同一接口查看日志；
左侧 USB Serial/JTAG 仍保留为次输出。ROM 下载态与应用 TinyUSB 态使用不同 USB 描述符，macOS/Linux
设备节点可能随重枚举变化，不要把 `/dev/cu.usbmodem*` 或 `/dev/ttyACM*` 名称写死在自动化中。

MicroPixel 应用 CDC 实现 BSP 的 DTR/RTS 自动下载握手。`flash-host` 识别应用 USB 产品名后请求芯片进入
ROM，随后按同一 USB 物理位置等待 ROM 产品名并在同一个 esptool 连接中完成芯片校验和写入。这里刻意不在
写入前单独运行 `chip-id`：rev0 S31 的一次性 ROM 下载状态会被探测连接关闭时的控制线变化消耗掉。该闭环已
在 macOS 真机验证；首次烧录、应用固件损坏或应用 CDC 未启动时，仍需按板卡说明手动进入 ROM 下载模式。

当前 `esp-mosaico` 第一阶段 profile 已接入 CO5300 显示、`78/esp_lcd_touch_cst92xx` 中断触摸组件、ES8311 音频、
BMI270、双 BMM150、BQ27220 主动刷新、POWER/Function Button、状态 LED、白名单扩展 GPIO、板级 3V3
电源、light sleep、共用 Runtime、BundleFS、native Wi-Fi、共享 App Hall/Status Layer 和 PPA/DMA2D
转场；RGB565/QSPI 只作为板级 presentation boundary，正常刷新和转场不使用 CPU 整图逐像素换序。
NAND、麦克风采集与模块发现仍未纳入当前范围；传感器轴向/磁校准和电源时序必须按下方真机清单验收。

第一阶段真机验收至少包括：静置/六面翻转检查加速度方向，绕三轴转动检查陀螺仪符号，两颗磁力计分别
读取且无串址；短按 POWER 完成睡眠与唤醒，长按触发关机；Demo Input 页中 Function Button 产生 Confirm
down/up 且 pressed/released 状态同步；Demo Devices 页选择 `Orange status LED` 后，TOGGLE 可点亮/熄灭且
退出页面恢复熄灭；插拔 Type-C/VIN 时电池与外部供电状态在 2 秒级更新；逐根检查公开 GPIO 不与显示、
触摸、音频、电源和调试脚冲突。
如果 ESP-IDF preview 自身出现源码/header 不同步，应更新或重装对应 SDK，不在项目仓库中修补本机 IDF。

## ESP32-S3 / ESP32-S3-BOX-3、立创 SZPI 与 M5Stack CoreS3 预览版

BOX-3 正式预览配置固定使用 40 MHz SPI、40 行 PSRAM partial buffer 和双缓冲：

```sh
bash tools/s3.sh build-host box3
bash tools/s3.sh build-release box3  # Host + Blocks/Snake/Tilt/Demo + 完整浏览器镜像
bash tools/s3.sh flash-all box3 /dev/cu.usbmodemXXXX
bash tools/s3.sh monitor box3 /dev/cu.usbmodemXXXX --reset
```

`build-release` 生成 `build/host-esp32s3-box-3/micropixel.bin` 和 `micropixel-full.bin`；完整镜像包含
四个 Xtensa AOT App。`flash-all` 烧录 Host 和对应的 8 MiB `app_store` 内容。烧录入口会先确认端口连接的是
ESP32-S3，默认使用原生 USB Serial/JTAG。

立创 SZPI ESP32-S3 使用同一套 40 行 PSRAM 双缓冲和 Xtensa Guest 基线，板级显示、触控与传感器接线
由独立 profile 提供：

```sh
bash tools/s3.sh build-host szpi
bash tools/s3.sh flash-host szpi /dev/cu.usbmodemXXXX
bash tools/s3.sh monitor szpi /dev/cu.usbmodemXXXX --reset
```

M5Stack CoreS3 使用 Quad PSRAM、ILI9342C/FT6336U、AW88298 和 AXP2101/AW9523 板级组合：

```sh
bash tools/s3.sh build-host cores3
bash tools/s3.sh flash-host cores3 /dev/cu.usbmodemXXXX
bash tools/s3.sh monitor cores3 /dev/cu.usbmodemXXXX --reset
```

`build-release`、`flash-apps` 和 `flash-all` 同样接受 `szpi` 或 `cores3`；不写 `BOARD` 时保持原行为，默认
操作 BOX-3。原有带 `-szpi`、`-cores3` 后缀的命令仍是兼容别名。

新板第一次接入必须先烧一次 `app_store`（`flash-all` 或 `flash-apps BOARD PORT`）。只 `flash-host` 的
新板 `app_store` 分区是空白 flash，Host 启动会打印 `App Store catalog scan failed`，
`micropixel app list` 返回 `count=0, storeUsedBytes=0`，此时 `app install`/`run` 的 Bundle 上传会长时间
挂起而没有明确错误。三款 S3 共享 `build/esp32s3-apps/app-store.bin`，`flash-apps` 不区分板型。

五板固件发布使用同一 `PROJECT_VER`，逐个生成 OTA `micropixel.bin` 与浏览器完整镜像
`micropixel-full.bin`：

```sh
bash tools/p4.sh build-release
bash tools/s31.sh build-release
bash tools/s3.sh build-release box3
bash tools/s3.sh build-release szpi
bash tools/s3.sh build-release cores3
```

发布目录由 Control 服务仓库的 `firmware-release.jsonc` 统一声明（Control API 与官网不在本仓库）。三款 S3
虽共享芯片和 Xtensa App Store，但 Host 镜像不可互换；设备 OTA 使用 Board profile 的 target，在线烧录页由用户
选择具体板型并只用芯片识别做系列校验。固件镜像属于生成物，不提交到 Git；部署网站/API 时必须让配置中的五组
相对路径都可读。

Host 与 Guest App 是两个独立更新通道。只修改固件时执行上面的 `build-host` + `flash-host`；只修改 SDK Demo
时使用产品固件的 USB 本地控制通道增量安装，不进入 ROM 下载态，也不重写 Host：

```sh
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemXXXX \
    app install guest/apps/demo
```

## 4. 完整烧录 Host 和八个示例 App

新设备或需要同时更新 Host 与 BundleFS 元数据时，使用：

```sh
bash tools/p4.sh flash-all "$P4_PORT"
```

该入口会：

1. 构建 Host 固件和 Blocks、Snake、Tilt、Demo 及四个 Showcase Bundle；
2. 生成包含八个 App 的 BundleFS 镜像；
3. 烧录 bootloader、分区表、OTA 初始数据和 Host 固件；
4. 清空并烧录 App Store，随后读回校验。

该命令不运行测试。发布前或推送前单独执行：

```sh
bash tools/p4.sh test
```

成功时命令末尾会输出 `System Shell P4 flashed on ... with eight Apps.`。脚本不再自动抓取启动日志，
用 `bash tools/p4.sh monitor "$P4_PORT"` 确认 `System Shell ready: App Hall rendered with apps=8`；
设备复位后应在 App Hall 中看到八个 App，并可在第一行左右滑动浏览。

## 5. USB 烧录八个示例 App

Host 固件和分区表未变化时，直接清空旧 App Store 并烧录已有的八个示例 Bundle：

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
python3 tools/micropixel --transport usb --port "$P4_PORT" app start micropixel.demo
python3 tools/micropixel --transport usb --port "$P4_PORT" app stop
python3 tools/micropixel --transport usb --port "$P4_PORT" app uninstall micropixel.demo
```

`micropixel --transport usb app install guest/apps/demo` 会先按正式流程构建和打包，再通过同一协议安装；
传入现有 `.bundle.bin` 时则直接安装。`app install` 和 `run` 会在终端显示安装进度：`0–99%` 跟随已确认的
Bundle 分块，设备完成校验、BundleFS 写时复制和 Catalog 提交后显示 `100%`。安装、升级和卸载仍由
HostController 串行执行；运行中的 Guest 必须先停止。Bundle 数据使用分块确认传输，
设备重新计算 SHA-256，并在完整 Bundle/AOT/资源校验通过后提交 BundleFS Catalog。

macOS/Linux 端口通常形如 `/dev/cu.usbmodemXXXX` 或 `/dev/ttyACM0`；Windows 端口形如 `COM7`，可直接传给
`--port`。端口自动探测同样按 ESP32 USB Serial/JTAG 的 VID/PID 工作。当前实现已在 macOS 真机验证；
Windows 使用 pyserial 的 COM 端口后端，代码路径受支持，但尚未完成项目真机验证。

该协议依赖正在运行的 Host 固件，不适用于下载模式或 bootloader。monitor、esptool 和本地控制共享板卡的
USB CDC 端口，不能同时占用。截图统一使用 `micropixel --transport usb screenshot`，在 P4 与 S31 上使用
相同的 JPEG framing，并只抓取当前显示提交缓冲。

## 7. Conformance 配置与串口调试

需要专用 conformance Host 时，仍通过统一入口显式叠加配置：

```sh
P4_SDKCONFIG_DEFAULTS="$PWD/firmware/espressif/sdkconfig.defaults;$PWD/firmware/espressif/sdkconfig.p4.defaults;$PWD/firmware/espressif/sdkconfig.p4-conformance.defaults" \
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

## 8. 验收与常见问题

完整烧录后至少检查：

- 终端中 Host 固件和 BundleFS 元数据均报告写入校验成功；
- monitor 中出现 `System Shell ready: App Hall rendered with apps=8`；
- App Hall 中可左右滑动浏览并启动八个示例 App；
- 状态栏中的亮度和音量控制生效。

常见失败：

- `IDF_PATH is not set`：当前终端尚未 `source` ESP-IDF 6.1 的 `export.sh`；
- `Serial port not found`：设备重新枚举后端口名已变，重新执行 `ls /dev/cu.usbmodem*`；
- `No serial data received`：选错 CDC 端口、USB 线不支持数据，或设备未进入可下载状态；
- 多台设备串口名重用：再次运行 `esptool chip-id`，以 MAC 而不是端口名确认目标。
- `micropixel app install`/`run` 卡在上传、`app list` 为 `count=0, storeUsedBytes=0`、启动日志有
  `App Store catalog scan failed`：该设备的 `app_store` 从未烧录。先执行对应板型的 `flash-apps`
  （P4 `bash tools/p4.sh flash-apps PORT`，S3 `bash tools/s3.sh flash-apps BOARD PORT`），再增量安装。
- 新增的 `CONFIG_MICROPIXEL_*` Kconfig 符号在已有构建目录里不生效（对应代码被整体编译掉、日志消失）：
  `build/host-<target>/sdkconfig.release` 是已物化的生成物，改 `Kconfig.projbuild` 后 ESP-IDF 6.1 不一定
  重新生成它。用 `rg CONFIG_MICROPIXEL_XXX build/host-<target>/sdkconfig.release` 确认；缺失时删除该
  `sdkconfig.release` 让下次 `build-host` 重新生成，或对该 build dir 执行一次 `idf.py reconfigure`。
- macOS 没有 `timeout` 命令；给 `micropixel run` 限时请用 `--no-follow` 加随后的 `logs -n N`，不要依赖
  `timeout`。
- ESP-MOSAICO `0.2.4` 或更早版本的 OTA 在 99% 报 `ESP_ERR_OTA_VALIDATE_FAILED`：这些预览固件早于多板型
  release target，更新检查会落到兼容旧 P4 的默认目录并下载 P4 镜像。先用 `bash tools/s31.sh flash-host`
  进行一次保留 NVS 与 `app_store` 的 USB Host 更新；`0.3.0` 及以后版本会显式请求 `esp-mosaico`，后续可
  正常 OTA。不要把服务器默认 target 改成 S31，否则会让同版本的旧 P4 设备下载错误镜像。
