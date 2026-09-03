# ESP32-S3-BOX-3 适配与能力探索指南

本文是 ESP32-S3-BOX-3 preview 的长期维护指南。它记录目标架构、历史验证阶段、RGB565 数据路径、
最大能力测量方法和完成门槛。当前产品仍是 ESP32-P4 + Metalio-Claw4；ESP32-S31 + ESP-Mosaico，
以及 ESP32-S3-BOX-3、立创 SZPI ESP32-S3、M5Stack CoreS3 是同步维护的 preview profile。

当前状态：P0 已完成编译、Bundle 和 BOX-3 真机验收；P1 已完成官方 BSP display/touch/backlight
接入、CPU-only RGB565 panel 路径和真机时钟/缓冲对照；P2 已完成 Guest Graphics、资源与 streaming
texture 的 RGB565 主路径，并在真机运行 Demo、Blocks、Snake。默认 profile 使用 40 行 PSRAM double
buffer，App Surface、layer cache、transform scratch、Scene 和资源大缓冲也使用 PSRAM，不为这些缓冲或
RGB565 wire stage 保留大块 internal SRAM。P3 已完成 320×240 landscape System Shell、PSRAM RGB565
displayed shadow、USB JPEG 截图和 Guest/Hall 生命周期真机验收。最终 SPI 时钟保持 40 MHz；80 MHz 因
显示异常被否决。P4 的第一批产品能力已经接入：原生 ESP32-S3 Wi-Fi 复用共享 `WifiManager`，扬声器
复用共享 ES8311/I2S sink，侧边 Mute 由 Host 独占并同时归零软件混音；能力初始化失败时保持 unavailable，
不阻断 display、USB 或 App Runtime 启动。扬声器可听与侧边 Mute 的立即静音/恢复已完成真机听感确认。
P5 的 AOT target 元数据、CLI 自动选 target 和安装写入前 compatibility preflight 已完成。板载 IMU、
Pmod GPIO、OTA、Web Console 和 preview 发布目录也已接入。P0/P1 等名称只表示下文的 BOX-3 历史验证阶段，
不再对应活动配置或命令；当前同时维护 `esp-box-3`、`szpi-esp32s3` 与 `m5stack-cores3` preview profile，并将 LVGL object
pool、relocated AOT text 和非映射 AOT package buffer 放入 PSRAM。

## 1. 目标与非目标

ESP32-S3-BOX-3 首先作为 preview profile 接入，用它回答两个问题：

1. MicroPixel 在 Xtensa ESP32-S3 上运行 AOT Guest 的稳定边界是什么；
2. 在 320×240 RGB565 SPI panel、CPU software compositor 和原生 Wi-Fi/音频并存时，实际可持续的
   Scene、LVGL 和 panel 吞吐上限是什么。

本阶段不把 60 Hz 当成 BOX-3 的验收上限，也不预设它必须达到 60 FPS。P4/S31 现有 60 Hz 配置和
基线保持不变；BOX-3 正常 profile 初期可以继续使用 16 ms 级调度，另设可重复的能力探索 profile，
让 LVGL invalidation、panel submit 或专用 benchmark 以完成事件驱动尽快继续，测出真实瓶颈。最终应同时
报告峰值、可持续值和真实 App 表现，不能只保留一个 FPS 数字。

首阶段不承诺：

- 与 P4/S31 共用同一个 AOT 文件；
- Guest 麦克风输入、SD 卡 Service、Bluetooth Service 或摄像头 Service；
- Guest↔App Hall 硬件缩放转场；
- 电池、震动马达或物理关机能力；
- 在测量前先人为限制刷新率、SPI 时钟或 Scene 提交频率。

## 2. 官方硬件基线

ESP32-S3-BOX-3 官方 BSP 给出的基础资源为 16 MiB QSPI Flash、16 MiB Octal PSRAM、320×240
2.4 英寸触控屏、双麦克风、扬声器、microSD 和两个 Pmod 兼容扩展口。ESP32-S3 主 CPU 是最高
240 MHz 的双核 Xtensa LX7，带单精度 FPU 和向量扩展。

长期维护只引用以下官方来源，不把原理图或数据手册副本提交到仓库：

- [ESP-BOX-3 BSP 与能力表](https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/README.md)；
- [ESP-BOX-3 BSP pin/capability header](https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/include/bsp/esp-box-3.h)；
- [ESP-BOX-3 display API](https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/include/bsp/display.h)；
- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)；
- [ESP32-S3 external RAM 约束](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html)。

官方 BSP 同时列出 ST7789/ILI9341 display 和 TT21100/GT911 touch 组合。实现不得假定所有 BOX-3
批次使用同一个 controller；优先复用并固定版本的官方 BSP 检测/初始化路径，再由 `device::BoardInfo`
报告真机实际选择。MicroPixel 板级目录只拥有 wiring、启动次序和适配层，不复制官方器件驱动。

## 3. 固定架构决策

### 3.1 新增独立 ESP32-S3 preview profile

新增板型仍遵守固定依赖方向：

```text
Runtime -> Device contracts <- Platform
```

早期曾使用 Null Board 验证 SoC、Runtime 和 Xtensa AOT；阶段性可烧录配置已在 preview 收口后删除。
当前只保留通用 `s3-null` 编译门禁和正式 BOX-3 preview：

```text
firmware/espressif/main/platform/
  targets/esp32s3/                 # SoC component 依赖
firmware/espressif/
  sdkconfig.defaults               # P4、S31、S3 公共基线
  sdkconfig.s3.defaults            # S3 SoC 基线
  sdkconfig.s3-null.defaults
  sdkconfig.s3-box-3.defaults      # BOX-3 preview
  partitions.s3.csv
tools/
  build_wamrc_xtensa.sh
  s3.sh
```

真实 BSP wiring 位于 `boards/esp32-s3-box-3/`。`tools/firmware_profiles.json` 注册不可烧录的 `s3-null`
和可烧录的 `esp-box-3` preview。构建、烧录、monitor 和芯片核验继续由 `tools/firmware.py` 完成；
板型 shell 只保留产品流程别名，不复制通用实现。

### 3.2 AOT 按 CPU 架构分别发布

现有 Guest 固定生成 `RISCV32_ILP32F` AOT，不能在 Xtensa LX7 上执行。WAMR Runtime 已能按 ESP-IDF
target 选择 `XTENSA`，但 `wamrc` 必须使用包含 Xtensa backend 的 Espressif LLVM 构建。编译器版本、
WAMR fork commit 和 AOT format 必须继续锁定并进入可重复构建门禁。

第一阶段采用一个 CPU 架构一个 Bundle：

```text
same Wasm + resources
  ├─ wamrc RISCV32_ILP32F -> riscv32 Bundle -> P4/S31
  └─ wamrc XTENSA         -> xtensa Bundle  -> ESP32-S3
```

`micropixel build/package` 接受明确的 AOT target；离线 `package` 要求显式指定。连接设备执行 `run` 或
`app install` 时，CLI 先读取设备芯片并自动选择 target；CLI 和 Host 安装入口都会在写入 `app_store` 前
拒绝无 target 元数据或架构不兼容的 AOT，不能等到启动时只返回 `aot_load_failed`。

Bundle v1 当前仍只允许一个 AOT section。AOT target bitmap 写在该 section 的 `reserved0`：bit 0 表示
RISC-V `ilp32f`，bit 1 表示 ESP32-S3 Xtensa，`0` 表示旧 Bundle 缺少元数据。当前每个 AOT section 必须
恰好设置一个已知 bit；新 App Bundle 必须写入非零 target mask；
已安装的旧 Bundle 仍可由 reader 打开，但再次安装时会被 preflight 拒绝并要求重建。这个布局为未来同一
Bundle 放多个 AOT section 留出了空间：每个 payload 可独立声明 target，不需要新增全局 header 字段或提升
容器版本，各 section mask 的 OR 即为 Bundle 支持的架构集合；真正启用多 target 前仍须单独设计目标选择、
重复 section 规则和容量策略。

### 3.3 RGB565 是 BOX-3 的原生工作格式

BOX-3 的目标不是把现有 BGR888 App Surface 整帧转换为 RGB565，而是让不含 alpha 的稳态路径从资源到
panel 都保持 RGB565：

```text
opaque raw asset / JPEG / opaque PNG
  -> canonical RGB565 texture
  -> RGB565 App Surface
  -> LVGL RGB565 draw/partial buffer
  -> RGB565 displayed shadow（如启用截图或恢复）
  -> panel IO byte-order adaptation
  -> RGB565 panel GRAM
```

内存中的 canonical 格式必须在 ABI 文档中固定，例如定义为 little-endian RGB565 word。某些 panel 需要
相反的传输字节序；由 SPI/panel IO 的硬件 swap、DMA swap 或最后的有界提交 stage 处理。字节换序不是
RGB888→RGB565 颜色转换，但仍要在 telemetry 中单独计数，不能声称完全零处理。

透明资源不能无损表示为普通 RGB565。第一阶段保持以下规则：

- opaque raw asset 在打包期直接生成 RGB565；运行时 Host 在加载时把它从 Bundle 的 flash 映射
  暂存到 PSRAM（PSRAM 不足才退回 flash 映射），避免绘制热路径反复经过 flash cache，见
  [`graphics-performance.zh-CN.md`](graphics-performance.zh-CN.md) 第 7 节；
- JPEG 直接请求 decoder 输出 RGB565，不先生成 RGB888；
- opaque PNG 优先在打包期转成 raw RGB565；保留压缩 PNG 时只在加载时做一次 RGB→RGB565 展开；
- 含 alpha PNG 保持 BGRA8888，绘制时直接 blend 到 RGB565 destination，不产生 RGB888 中间整帧；
- Scene fill/text 继续使用逻辑 RGB888 color value，在写目标 pixel 时一次量化为 RGB565；
- scale/rotation 直接从源格式写入 RGB565 destination，scratch 只覆盖固定行批次，不创建全尺寸 RGB888
  临时图。

P2 开始前，平台内部 `PixelCompositor` 和 LVGL software kernels 已有 RGB565 destination 能力，但公开
边界尚未完整支持它：

- `micropixel_pixel_format_t` 只有 BGR888/BGRA8888；
- Bundle raw asset 只有 BGR888/BGRA8888；
- `ResourceService`、`BitmapStore`、streaming texture 和 Guest SDK validation 只接受这两种格式；
- `GuestGraphicsEngine::GetInfo()` 固定报告 BGR888；
- App Surface allocation 和 `BitmapView` 转换仍把每像素字节数限定为 3 或 4。

P2 已按下面的跨 ABI、Bundle、Runtime、Platform 和 SDK 向后兼容扩展完成；它不是板级 driver 私有优化：

1. 为 RGB565 追加新的、不可复用的 pixel-format ID，并更新 ABI README、SDK enum 和 conformance；
2. 为 raw RGB565 追加新的 Bundle asset format，定义 stride、size、endian 和 hash 验证；
3. 扩展打包器支持 `raw_rgb565` 与 `png_to_raw_rgb565`，不改变既有 BGR/BGRA 资源含义；
4. 让 BitmapStore、Resource Service、adaptive scale 和 streaming texture 接受 2 B/px；
5. 由 Board/Graphics capability 选择 App Surface destination，P4 继续 RGB888，BOX-3 选择 RGB565；
6. 让 Guest SDK 接受设备报告 RGB565，但保持逻辑坐标、Scene color 和现有 App 源码兼容；
7. 为格式错误、stride 溢出、奇数字节长度、alpha 丢失和跨格式 scale/blend 增加负向测试。

不要为简化 S3 实现而改变既有 pixel-format ID、Bundle format ID 或 P4/S31 的解释。

### 3.4 S3 使用真正的 CPU-only compositor

ESP32-S3 不提供本项目在 P4/S31 上使用的 PPA/DMA2D。共享 `platform/lvgl/` 当前仍直接编译和持有
`EspPixelCompositor`，S3 适配必须把 concrete accelerator 从 `GuestGraphicsEngine` 解耦：

- `GuestGraphicsEngine` 依赖 `PixelCompositor` contract 或 target-selected backend；
- P4/S31 继续选择 PPA/DMA2D backend；
- S3 只编译 `LvglSoftwarePixelCompositor` 和 reference fallback，不 include `driver/ppa.h`；
- S3 不提交 `DisplayTransition`，共享 Host UI 使用直接切换；
- SPI DMA 只负责 panel transport，不被统计成 PPA/DMA2D 图形加速；
- 小脏区、等尺寸 opaque copy、alpha blend 和 scale 分开计数，以便识别真实 CPU 热点。

ESP32-S3 的向量扩展是否能被当前 ESP-IDF/LVGL 路径利用，必须以反汇编和真机 A/B 为准。不能因为芯片
有 SIMD 就宣称 software compositor 已经向量化，也不能为了微基准引入不受支持的 LVGL asm profile。

### 3.5 320×240 Host UI 使用独立完整 profile

Guest 的逻辑显示变换已经支持横屏，320×240 会映射成 960×720 逻辑空间，因此应用不应重新引入
board-specific layout。需要新增的是 Host-owned UI profile：

- 抽取 `square_common` 中与屏幕形状无关的 Hall、页面和生命周期状态；
- 新增完整的 320×240 landscape Hall、状态栏、系统菜单、系统详情页、Wi-Fi 键盘和 Status Layer 几何；
- 不把 320×240 根据“宽度小于 480”错误套入 480×480 profile；
- 初期只做直接 Guest/Hall 切换，截图和动画作为独立可选 presentation role；
- 所有触控 target、滚动边界、文字截断和安全区都用 Host test 固定。

板级 `presentation.*` 只能提供 display、brightness、screen capture 等呈现能力，不得复制 App Hall 页面和
HostController 状态机。

## 4. 分阶段实施计划

### 历史 P0：ESP32-S3 与 Xtensa 编译基线

目标是证明 Runtime/Device 不依赖 RISC-V 或现有两块板。

- 放开顶层 ESP-IDF target whitelist，增加 `targets/esp32s3/`；
- 增加 `s3-null`、独立 build directory 和 sdkconfig defaults；
- 构建锁定版本的 Xtensa `wamrc`；
- 用同一份 conformance Wasm 分别生成 RISC-V/Xtensa AOT；
- 在 S3 Null Host 上通过 ABI version、log、clock、event 和最小 SDK Guest；
- 验证 AOT text 位于 internal SRAM 与 executable PSRAM 两种模式，记录容量、literal relocation、启动时间
  和 Flash 写入并发限制。

P0 完成前不开始复制 Board 外设代码。

当前正式执行入口如下，`PORT` 必须是经芯片核验的 BOX-3 USB Serial/JTAG 端口：

```sh
# 通用编译门禁和 Xtensa compiler
bash tools/s3.sh build-null
bash tools/s3.sh build-wamrc

# 三款 S3 preview；BOARD 为 box3、szpi 或 cores3
bash tools/s3.sh build-release BOARD
bash tools/s3.sh flash-all BOARD PORT
bash tools/s3.sh monitor BOARD PORT --reset
```

`monitor` 默认不复位，适合查看正在运行的固件；需要完整启动日志时显式传 `--reset`。若原生 USB
Serial/JTAG 的自动复位使芯片进入 ROM download，应退出 monitor、重新附加且不传 `--reset`，再短按板上
RESET。不要反复烧录 `app_store` 规避复位问题。

最小 Guest 构造 `Application`，但不请求 Renderer、显示或输入；SDK 仅在真实需要显示坐标的操作上按需
打开 Graphics Service。因此 P0 不注册虚构 display，也不把 Null Board 当作 BOX-3 display bring-up。
Guest 必须依次证明 ABI startup、log、clock、`event_wait` 和 Timer Service，最后正常返回并完成
`AppSession` 清理。两种 placement 都必须看到同一条 `P0 PASS`，且不能出现 trap、panic、重启循环或
AOT load/relocation 失败。PSRAM profile 还必须由运行时日志证明执行地址与写入镜像地址分离，写入镜像
实际来自 PSRAM。

P0 的数据只用于证明“能正确运行”和比较 AOT placement，不代表图形性能。Display/touch、RGB565 panel
吞吐与最大刷新能力从 P1 开始测量。

当前 P0 基线已经在 ESP32-S3-BOX-3 真机上通过，结果如下：

| 项目 | internal executable SRAM | executable PSRAM |
|---|---:|---:|
| Host image | 1,056,336 B | 1,056,768 B |
| Xtensa AOT / Bundle extent | 47,180 B / 65,536 B | 同一份 Guest |
| AOT executable text allocation | internal D/IRAM | 31,292 B，PSRAM D-bus 写入、I-bus 执行 |
| 单次功能验收 Guest entry | 约 101.6 ms | 约 101.9 ms |
| ABI/log/clock/event_wait/Timer | PASS | PASS |
| Guest return 与资源清理 | PASS | PASS |

这里的 entry 时间只用于 P0 烟测量级和 placement 对照，不是性能结论；P1/P2 必须使用预热、多轮统计和
持续负载重新测量。P0 真机还确认了 16 MiB Flash、16 MiB Octal PSRAM、240 MHz 双核启动基线和
8 MiB `app_store` 分区。仓库不保存设备标识或原始串口日志。

Xtensa AOT 首次真机 bring-up 暴露并修复了三类必须长期保留的约束：

- GCC 优化下不得用违反 strict-aliasing 的指针重解释写 Xtensa 16-bit relocation；当前实现用对齐 word、
  `memcpy` 和 volatile word store，避免正常路径被优化掉；
- ESP32-S3 internal executable D/IRAM 与 executable PSRAM 都必须通过 D-bus mirror 写入 AOT text 和
  relocation，再从 I-bus alias 执行；
- 写完 code 后必须执行 Xtensa `memw`/`isync`，不能依赖在该工具链上为空操作的通用
  `__builtin___clear_cache`。

运行期间 AOT package 仍映射自 `app_store`。HostController 已固定为只有
`AppLifecycleState::kNotRunning` 时才允许安装或卸载，避免 Guest 持有 Flash mapping 时并发擦写；S3
profile 不得绕过这一门禁。PSRAM placement 只移动已装载的 executable text，不改变 BundleFS/Flash
并发规则。

### 历史 P1：最小 display/touch 与 panel 上限

目标是先测清 SPI panel，不让 App Hall 或资源系统掩盖 transport 极限。

- 以 Apache-2.0 官方 BSP 为硬件事实来源，将显示、触摸、I2C 和背光所需的最小板级逻辑维护在
  `platform/boards/esp32-s3-box-3/`，避免整包 BSP 约束公共依赖版本；
- 初始化实际 display/touch controller、PWM backlight 和 USB Serial/JTAG console；
- 注册 RGB565 partial display，不启用 PPA/DMA2D 或系统转场；
- 接入通用 `EspLcdTouchInput`、坐标旋转和中断唤醒；
- 实现只包含色块、棋盘、滚动条和连续 full-screen invalidation 的 Host benchmark；
- 比较不同 SPI clock、draw-buffer height、single/double buffer、internal DMA stage 与 PSRAM source；
- 测出无撕裂/无丢 flush 的峰值和至少 10 分钟可持续值。

P1 不要求完整 App Hall，也不把官方 demo 的 FPS 当作 MicroPixel 结果。

该阶段最终收口到当前单一 preview profile，不向 P4/S31 的 Graphics Service 或 60 Hz 配置扩散：

```text
firmware/espressif/main/platform/boards/esp32-s3-box-3/
  platform.cpp                 # BSP、LVGL adapter、touch、backlight 与能力注册
firmware/espressif/
  sdkconfig.s3-box-3.defaults
```

硬件初始化参考 Apache-2.0 的官方 `espressif/esp-box-3` BSP 3.2.0，但不链接整包 BSP。显示和触摸使用对应 controller
探测路径；已验收真机选择 ILI9341 + GT911。Panel 总线保持 BSP 支持的 40 MHz，背光由 BSP PWM
设为 80%，console/local control 继续使用原生 USB Serial/JTAG。P1 不注册 Guest Graphics，也不启用
PPA/DMA2D 或系统转场。

LVGL source、benchmark 和 panel 都使用 RGB565。ILI9341 wire order 仍要求每个 16-bit pixel 做字节
换序。ESP32-S3 路径直接在 PSRAM draw buffer 上原地换序，再由 SPI LCD DMA 从 PSRAM 提交，不分配
panel-strip-sized internal wire stage；这是 byte-order packing，不是 RGB888→RGB565 颜色转换。PPA
targets 保留既有硬件路径，S3 不 include PPA driver。

当前唯一的 BOX-3 preview 执行入口如下：

```sh
# 当前默认：40 行、PSRAM source、double buffer、40 MHz
bash tools/s3.sh build-host
bash tools/s3.sh flash-host PORT
bash tools/s3.sh monitor PORT
```

40/80/240 行、single/double buffer 的 A/B 已结束；测试 profile 与 PanelBenchmark 源码已清理，下面只
保留测试结论。正式配置固定为 40 行、PSRAM source、double buffer、40 MHz；80 行只提高约 1.7% FPS，
不足以抵消额外常驻的 50 KiB PSRAM。

2026-09-01 在同一块 BOX-3、40 MHz SPI、scrolling-bar 全屏 invalidation 下，各取启动后的连续稳定
5 秒窗口，结果如下。两组均为 PSRAM source，且未观察到异常计数、错色或画面破损：

| draw buffer | FPS | wire | flush/frame | render/frame | refresh/frame | byte-swap/flush |
|---|---:|---:|---:|---:|---:|---:|
| 40 行、double | 21.85 | 26.85 Mbit/s | 6 | 43.55 ms | 44.09 ms | 0.66 ms |
| 240 行、single | 19.80 | 24.32 Mbit/s | 1 | 48.37 ms | 48.92 ms | 4.37 ms |

240 行 single 虽把每帧 flush 从 6 次减到 1 次，但失去了 double buffer 提供的 CPU render 与 SPI transfer
重叠，最终 FPS 下降约 9.4%，refresh frame time 增加约 11.0%，同时多占 100 KiB PSRAM。因此产品使用
40 行 double；240 行 single 不再作为活动配置。

以下是 P1 阶段早期、不含当前完整 System Shell/网络负载的真机记录，用于保留 SPI 时钟探索结论，不能与
上面的当前固件 buffer A/B 数值直接比较。`wire` 是 RGB565 pixel payload，未把 SPI command/地址开销算进
有效载荷；表中数值是预热后的 5 秒稳定窗口，不是 60 Hz 限制值。时钟探索直接修改板级实验常量并增量
重编 BSP/板级文件，不建立每个时钟一份 sdkconfig/build directory：

| profile | source / partial buffer | 实测 FPS | RGB565 payload | 结论 |
|---|---|---:|---:|---|
| 当前 preview | PSRAM，40 行，double，40 MHz | 27.8 | 34.1 Mbit/s | 正式配置；画面正常 |
| 时钟探索 | PSRAM，40 行，double，请求 60 MHz | 27.8 | 34.1 Mbit/s | 与 40 MHz 表现相同，未形成独立有效档 |
| 时钟探索 | PSRAM，40 行，double，80 MHz | 43.5 | 53.4 Mbit/s | 真机出现显示异常，拒绝使用 |

40 MHz 默认保持 full-screen RGB565、`anomalies=0`，未出现 watchdog、panic 或重启，CPU 原地 byte
swap 平均约 0.30 ms/40-row flush。80 MHz 虽显著提高吞吐，但软件计数无法发现信号完整性造成的画面
异常，因此必须以人工真机观察否决；不能仅凭 `anomalies=0` 把它选为默认。当前 BSP/默认 SPI 时钟源下
保留官方 40 MHz。10 分钟持续验收累计 16,295 帧、97,770 次分块刷新和 1,251,456,000 像素，
`P1 SUSTAIN PASS` 的 `anomalies=0`；期间未出现 watchdog、panic 或重启。仓库只记录这些聚合值，
不保存原始串口日志或设备标识。

P1 的 partial-buffer 架构没有完整 displayed shadow，因此 `MICROPIXEL_CAPTURE` 在该 profile 明确返回
unavailable，不能把最后一条 partial strip 误当成整屏。截图、完整 320×240 System Shell 和 Guest
Graphics 分别在后续阶段决定是否保留 full-frame RGB565 shadow。P3 开始时已补齐该能力，下面的 P1 描述
保留为当时的历史基线。

### P2：RGB565 Guest Graphics 与资源直通

状态：已完成代码接入和 Demo、Blocks、Snake 真机验收。实现保持既有 wire ID 含义，并追加
`MICROPIXEL_PIXEL_FORMAT_RGB565=3` 和 Bundle raw format `10`；canonical memory order 是 little-endian
RGB565。BOX-3 通过 board-owned `kGuestSurfaceFormat` 选择 RGB565，P4/S31 不随之改变。

- App Surface 和 layer cache 均为 320×240×2 bytes，分别占 153,600 bytes；20,480-byte transform scratch、
  118,784-byte Scene 及资源大缓冲也明确位于 PSRAM；
- JPEG 可直接 decode 到 RGB565；opaque PNG 可在打包期转换为 raw RGB565，运行时保留压缩 PNG 时只在
  load 阶段转换一次；带 alpha PNG 保持 BGRA8888；
- BGRA8888 alpha resource 直接 blend 到 RGB565 destination，不创建整帧 RGB888 中间面；
- Resource Service、BitmapStore、adaptive scale、streaming texture 和 partial update 都接受 2 B/px；
- Blocks 的 300×150 streaming playfield 已切成 RGB565。初次 200 个 30×30 cell update 合计 360,000
  bytes，证明 payload 按 2 B/px 传输；后续局部更新约 9–12.6 KiB/scene；
- 320×240 横屏继续暴露 960×720 逻辑空间，Blocks/Snake 使用居中的 720×720 content viewport，输入坐标和
  layer translation 使用相同 offset；Host 校验 translated clip 必须完整留在逻辑 canvas 内；
- telemetry 分开记录 software blit、RGB888→RGB565 像素、alpha 像素、panel bytes、byte swap 和 refresh。
  Demo 首帧、Blocks/Snake 的透明素材仍会产生局部颜色量化或 alpha blend，这是预期行为；没有把全屏
  BGR888 App Surface 每帧转换成 RGB565。

真机结果使用最终 40 MHz SPI、PSRAM double buffer 和 release Xtensa AOT。Demo 能启动并响应触控；
Blocks 能从 Start 进入游戏、绘制 raw RGB565 board 并持续做 RGB565 streaming partial update；Snake 能
进入游戏并响应滑动，当时的 raw RGB565 board 与 BGRA sprite atlas 可同时显示。P3 UI 整理后，Blocks
棋盘背景已并入 RGB565 streaming surface，Snake 的纯色棋盘背景已改为 `RoundedRectNode`，两者都不再携带
独立 board 图片资源。BOX-3 当前 presentation 没有完整
displayed shadow，因此 P2 验收时屏幕截图命令仍返回 unavailable；P3 开始后已由 PSRAM RGB565 shadow
补齐截图，不改变上述 P2 性能数据。

#### RGB888 与 RGB565 App Surface A/B

为了只比较 App Surface 格式，A/B 仅改 board-owned 常量，保持同一块板、40 MHz、PSRAM、Host 代码和
Snake bundle，不改 `sdkconfig`。结果如下：

| App Surface | surface | layer cache | ready→首个 scene | scene #3→#120 | 持续 scene FPS |
|---|---:|---:|---:|---:|---:|
| BGR888 | 230,400 B | 230,400 B | 约 34 ms | 2,055 ms | 约 56.9 |
| RGB565 | 153,600 B | 153,600 B | 约 28 ms | 2,051 ms | 约 57.0 |

Snake 的稳态主要是小 dirty rectangle，panel refresh 而非整帧颜色转换占主导，所以两者持续 FPS 在测量
精度内相同。RGB565 仍值得作为默认：Surface + layer cache 合计少占 153,600 bytes PSRAM，首帧更快，
并消除了 opaque 整帧 presentation 的 24→16-bit 转换风险。`panel-submit` 当前 adapter 未形成隔离的 App
计数，byte-swap 计数也可能包含 Hall 流量，因此这两个字段不用于本次 A/B 结论。

### P3：320×240 System Shell

目标是提供完整但无硬件转场依赖的 Host UI。该阶段已经完成；BOX-3 启动路径只创建正式 System Shell，
不再包含 PanelBenchmark。

当前基础：已增加 320×240×2 bytes 的 canonical little-endian RGB565 displayed shadow，固定分配在
PSRAM。LVGL flush wrapper 在 panel transport 原地 byte swap 之前逐区域更新 shadow；ESP32-S3 使用
`esp_new_jpeg` 软件编码器直接编码 RGB565_LE，输入、输出和 shadow 大缓冲均不占 internal SRAM。Host
benchmark 与 Guest 画面都已通过 USB `screenshot` 真机验收，可用于后续逐页视觉回归。

BOX-3 的语义字体角色映射为 Small 10、Medium 12、Large 14、Title 18；P4/S31 保持 14、18、24、32。
这项映射同时服务 Host UI 和 Guest system-font handle，不改变 ABI。Hall、状态层、菜单、Wi-Fi 页面和 Guest
Demo 使用 displayed shadow 截图逐页校准；320×240 profile 用独立的紧凑字号阶梯和紧凑文案，减少卡片、状态
文本和空状态的横向截断风险。新增的 10/12 px 字库是 board profile 的补充变体，不改变其他板型的默认字号。
Guest Runtime 原样传递 Small/Medium/Large/Title，不再按 logical-to-physical scale 将角色降一级；实际像素字号由
Host profile 唯一决定，避免 BOX-3 重复缩小。ESP-Mosaico 的既有 Guest 有效字号在 Host Guest resolver 中兼容。
SDK Demo 首页的六个模块 label 跨分辨率统一使用 Medium；应用应按控件语义选择字体角色，不在 Guest Runtime
中进行全局字号降级。

- 新增 landscape profile 和布局测试；
- 接入 App Hall、Status Layer、系统菜单、Wi-Fi 页面、亮度和主音量；
- 保持系统手势和 Guest suspend/resume 语义与现有板一致；
- 封面 cache 选择 RGB565，透明 UI 只在最终目标上 blend；
- 保留 PSRAM RGB565 displayed shadow 和 JPEG screen capture，持续记录截图延迟与峰值 PSRAM；
- 完成触控边缘、快速滚动、键盘、长标题和最大 App 数量验收。

已完成的真机生命周期链为：Hall 启动 Guest → 顶边下拉 Status Layer → 关闭后返回同一 Guest → 底边上滑
暂停到 Hall → 点击 RUNNING/PAUSED 卡片恢复同一 Guest。Xtensa AOT loader 在写入 executable PSRAM 后，
必须通过 ESP-IDF cache API 只失效受影响的 I-cache line；不得直接关闭共享 I-cache，否则另一个核心正在从
PSRAM 绘制 LVGL 启动画面时会触发 cache error。

### P4：产品能力逐项接入

按独立能力接入，初始化失败时不伪造可用设备：

- native Wi-Fi：复用 `WifiManager` 和 `NativeWifiRadio`；
- USB local control：复用 USB Serial/JTAG transport、安装、日志和触摸注入；
- audio output：把 Mosaico 的 ES8311/I2S 实现抽成可配置共享 sink，再填写 BOX-3 pin/PA 时序；
- buttons：只把真实适合 Guest 的按键归一化为 Key；mute、亮度等系统功能仍由 Host 拥有；
- IMU：板载 ICM-42607-P 通过共享 I2C executor 注册加速度和角速度 Sensor；
- Pmod GPIO：排除原生 USB 的 GPIO19/20 和 Dock I2C 的 GPIO40/41，开放其余 12 路静态白名单；
- 温湿度 AHT30 位于外接 ESP32-S3-BOX-3-SENSOR 扩展板，不是 BOX-3 主机内建设备，本 profile 不注册；
- microSD、麦克风和 Bluetooth 保持未开放，直到各自有独立 Service/安全设计。

第一批“联网与扬声器闭环”已经落地，具体边界如下：

- BOX-3 注册 `NativeWifiRadio` 和共享 `WifiManager`，扫描、保存网络、重连、状态事件和 SNTP 沿用产品
  Host 语义，不增加 Guest 网络 ABI；Wi-Fi 后台工作使用统一 background executor；
- 自动发现先按已保存 profile 的信道做低干扰 passive scan；若所有已知信道都未找到保存的 SSID，立即追加
  一次 active all-channel fallback，再进入 60 秒起步的退避。这样 AP 重启后变更信道不会让设备永远只扫描
  旧信道；用户主动扫描若在 discovery 期间到达，仍优先接续完整用户扫描；
- Mosaico 原有 ES8311 控制与 stereo I2S 输出已抽到 `platform/audio/`。板目录只提供 7-bit codec 地址、
  I2C/I2S pin、PA 极性/电压和 DMA 几何；codec 与 touch 继续通过共享 I2C executor 串行访问；
- BOX-3 的 codec rail 常开，因此注册 audio capability 前先通过共享 I2C executor 探测 ES8311 ACK；探测
  失败时不注册伪造的 audio output，AudioEngine 任务创建失败也只降级为 unavailable，不阻断平台启动。
  Mosaico 的 codec 是按需上电，继续在电源开启后初始化，不做启动期假阴性的探测；
- BOX-3 输出保持 16 kHz stereo I2S，Guest tone/PCM、Host 对数主音量和前台 App 音频生命周期继续由
  `AudioEngine` 统一管理；codec 不可用时不向 `DeviceServices` 注册伪造的 audio output；
- GPIO1 的侧边 Mute 是 Host 控制，不发布为 Guest Key。硬件静音期间 mixer 始终输出零，但不覆写用户保存的
  主音量；解除静音后恢复原来的主音量语义；
- ES8311/I2S 的 128-frame 转换块、AudioEngine 的 output/双路 PCM scratch、Audio/Opus/I2C/USB 中不发起
  Flash 操作的任务栈和 Wi-Fi scan workspace 都是 PSRAM-only，分配失败即把对应能力标记为 unavailable，
  不回退挤占 internal SRAM。Guest 主线程仍使用 internal SRAM stack，因为 Bundle/Resource 路径可能在
  Flash cache 关闭期间运行；I2S/Wi-Fi 驱动明确要求的 DMA descriptor 和硬件 buffer 也继续留在 internal
  memory。displayed shadow、LVGL draw buffer、App Surface、截图和本地/远程控制的大缓冲固定在 PSRAM；
- 三块板都显式启用 LVGL builtin allocator 的固定 TLSF object pool PSRAM policy。BOX-3 在 `lv_init()`
  时用 explicit caps 分配，不依赖 external-BSS；P4/S31 使用 `.ext_ram.bss` placement。三块板不要求使用
  同一种链接机制，但都不再让该固定 pool 占用 internal SRAM；
- BOX-3 必须在 displayed shadow、Guest Graphics display events 和 System Shell 对象全部注册后才启动
  LVGL worker。LVGL builtin TLSF 不是并发 allocator；若先启动 worker、再由主任务无锁改 flush/event
  callback，冷启动 refresh 会与主任务的 LVGL allocation 竞争并破坏 PSRAM free list，表现为
  `block_locate_free` 断言后 task watchdog。修正初始化顺序后必须用连续硬复位覆盖该启动窗口；
- BOX-3 不再走 BSP 默认的 PSRAM→internal 临时 SPI copy。板级 panel IO 显式启用 direct PSRAM SPI DMA，
  40-row RGB565 block 直接从 LVGL PSRAM draw buffer 发送；SPI 仍保持已验证的 40 MHz，不改 `sdkconfig`；
- USB local control 已在 P3 共用，不另建 BOX-3 transport。Mute、Wi-Fi 和音频初始化都必须失败安全，
  不能阻止 Hall、截图、触摸注入和已安装 Guest 启动。

第一批真机验收至少包含：冷启动看到 `radio=native` 与 ES8311 ready；Wi-Fi 页面能完成扫描并连接一个网络；
启动带音效的 Guest 后 I2S 进入 active，扬声器可听；切换侧边 Mute 时立即静音，切回后音量恢复；Suspend 或
Stop 后按既有 grace policy 回到 idle。验收记录不提交 SSID、密码、MAC、串口路径或原始日志。

当前 BOX-3 已完成其中的自动化和可观测部分：冷启动确认 native radio、ES8311 ACK、codec 与 16 kHz stereo
I2S 初始化成功；Wi-Fi 页面扫描能返回多个接入点，连接状态为 enabled/available/connected，受控重启后约数秒
内自动恢复 connected；Snake 前台运行时 audio task 持续 active，停止 App 后任务退出且 internal SRAM 恢复
稳定。扬声器可听、侧边 Mute 的立即静音以及解除后恢复原音量均已通过现场听感确认，完整扬声器真机闭环
标记为通过。

板载 ICM-42607-P 已在真机启动时读到 `WHO_AM_I=0x60`，加速度和角速度分别注册为 built-in Sensor；Pmod
白名单的 12 路 GPIO 已注册。GPIO19/20 继续归原生 USB Serial/JTAG，GPIO40/41 继续归 Dock I2C，避免
Guest 抢占系统总线。

BOX-3 没有 MicroPixel 当前产品板的电池、震动马达和电源 latch；对应能力保持 unavailable。低功耗可单独
支持 display off/light sleep，但不得把“进入休眠”报告为物理关机。

### P5：分发、回归与文档收口

- [x] CLI 根据设备芯片选择 AOT target，离线 package 必须显式 target；
- [x] 安装前做 AOT compatibility preflight；
- [x] 16 MiB Flash 使用双 3.5 MiB OTA 分区和 8 MiB `app_store`，Xtensa Host 镜像空间通过构建校验；
- [x] 固件 OTA、Remote Control 和 Web Console 使用板型级 target；三款 S3 分别为 `esp-box-3`、
  `szpi-esp32s3` 和 `m5stack-cores3`，不再只凭芯片推断；
- [x] `build-release` 生成 Host、Demo/Blocks/Snake/Tilt 四个 Xtensa AOT App 和浏览器完整镜像，发布目录包含三款 S3 preview；
- BundleFS 在 S3 MMU page、跨块 mmap、安装和卸载路径通过；
- P4/S31 的 RISC-V package、RGB888/RGB565 presentation 和现有 60 Hz 基线不得回归；
- 更新 README、架构、Firmware、Guest、烧录和第三方 notices。

OTA rollback 和 BundleFS 断电恢复是跨板通用功能，不属于本次 S3 preview 收口范围，留到通用功能开发处理。

## 5. 最大能力测量矩阵

能力探索不能只运行一个 60 Hz App。至少分为四层，逐层找到瓶颈。

### 5.1 Panel transport

绕过 Guest 和复杂 LVGL 页面，连续提交已准备好的 RGB565 buffer：

- full screen 320×240；
- 1、8、16、32、64、120、240 行 strip；
- 单区域与多个离散 dirty region；
- PSRAM DMA source；仅在 SoC/接口不能直接 DMA 时才评估 PSRAM→internal bounded stage；
- single/double buffer；
- BSP 支持且信号稳定的各档 SPI clock。

记录 panel submit count、pixels、bytes、queue wait、DMA time、flush-ready 间隔和错误/撕裂。理论带宽只用于
解释结果，不能代替真机计时。

### 5.2 LVGL/CPU compositor

分别测量 RGB565 destination 上的：

- opaque fill；
- RGB565 等尺寸 copy；
- BGRA8888 alpha blend；
- RGB565 scale；
- text/glyph；
- 多 dirty region 合并；
- full-screen Host root composition。

每项记录 ns/pixel、固定 submit 成本、CPU、PSRAM 带宽和 cache 影响。必须区分 compositor 时间和 panel
flush 时间。

### 5.3 Guest Scene

使用相同 release Host 和 Xtensa AOT，至少覆盖：

- 静态页面，只在输入时更新；
- Snake 普通移动的小 damage；
- Blocks 的多矩形和文字；
- 粒子/alpha 场景；
- full-screen scroll；
- streaming texture partial update；
- 大面积 scale/rotation。

能力探索 Guest 可以使用完成事件驱动或可配置短周期，不把 60 Hz timer 当上限。正式 App 的 60 Hz 行为
另作用户体验基线。记录 Scene submit、presented refresh 和 panel flush 三个不同速率，避免把被合并的 Guest
提交误报成显示 FPS。

### 5.4 并发与可持续性

在最高稳定图形负载下依次加入：

- 16 kHz audio output；
- Wi-Fi connected idle、scan、下载和 Bundle install；
- PNG/JPEG decode；
- screen capture；
- App suspend/resume 和 Hall 切换。

至少记录 10 分钟窗口的平均值、P50/P95/P99、最大值、audio underrun、触控延迟、heap/最大连续 PSRAM、
watchdog、非法 cache access、panel error 和温度/降频状态。最大可持续配置必须无画面损坏、无音频爆音、
无 watchdog、无内存持续下降；短时峰值单独报告，不得当作产品能力。

测量方法和日志保留规则继续遵守 [Graphics 性能诊断](graphics-performance.zh-CN.md)：仓库只保存可复现的
聚合 baseline，不提交原始串口日志、设备标识或一次性 trace。

## 6. 主要风险与验证重点

### Xtensa executable PSRAM

WAMR fork 允许 ESP32-S3 把 relocated AOT text 放进 executable PSRAM，但已经明确提示 Xtensa literal
relocation 可能无法从外部地址到达 Runtime symbol。必须同时验证 internal executable allocation 和 PSRAM
I-bus/D-bus mirror；若大 Guest 只能使用 internal SRAM，应降低 AOT/linear-memory 策略或先用 interpreter
诊断，不能静默回退后仍宣称 AOT profile 完成。

P2 真机已验证 executable PSRAM 的双地址 mirror，Demo、Blocks、Snake 的 AOT text 都从 PSRAM 执行。
另外确认了一个独立的编译约束：Xtensa `l32r` 只能引用前方 256 KiB 内的 literal。WAMR `wamrc` 的
`--size-level=3` 对大模块没有插入足够的 literal island，会在 load 时报告 target address out of range；
这不是 PSRAM mirror 本身的错误。Xtensa package 固定覆盖为 `--size-level=0`，让 LLVM 为大模块生成 literal
island；RISC-V profile 继续保留原来的 size level。CLI 单元测试固定这项 target-specific 策略。

Host 与 `app_store` 可以独立烧录，因此排查 Guest 指令异常时必须先核对设备实际装载的 AOT text size，不能
只看本地 bundle 总大小。一次 Snake `InstrFetchProhibited` 被确认来自设备上遗留的旧 Xtensa AOT；重建并
单独烧录当前 P2 app store 后，多轮完整游戏和资源/audio 并发保持稳定。该结果不表示 loader 可以接受旧
产物，发布/真机验收仍必须把 Host 与架构匹配的 app store 视为同一基线。

ESP32-S3 的 Flash 和 PSRAM 共享 cache 资源。BundleFS 写入、NVS、OTA 与运行在 PSRAM 的 AOT/数据之间必须
按官方 external-RAM 约束做真机测试。正常安装流程优先停止 Guest，再写 BundleFS；任何允许运行中 Flash
写入的入口都必须证明 cache/stack/ISR 安全。

### Internal SRAM 预算

ESP32-S3 的 512 KiB SRAM 不是一个全部可由 `heap_caps_malloc()` 使用的连续堆。代码 IRAM、静态
DRAM、cache/地址映射、系统保留区和对齐先占用空间，启动日志或 diagnostics 中的 `total` 只表示最后注册
到 internal heap 的容量。BOX-3 第一批 Wi-Fi/音频接入后的旧 map 中，主要静态占用为：

- `.iram0.vectors + .iram0.text` 约 108 KiB；
- `.dram0.data` 约 26.6 KiB；
- `.dram0.bss` 约 128.8 KiB，其中 LVGL builtin object pool 单独占 64 KiB。

因此旧固件报告约 203.8 KiB internal heap，不代表芯片只有 203.8 KiB SRAM。把 LVGL object pool 改为
PSRAM explicit allocation 后，`.dram0.bss` 从 `0x20360` 降到 `0x10360`，internal heap 从 208,700 bytes
增加到 274,236 bytes。真机空闲 Hall 的 free internal SRAM 为 123,228 bytes；启动 Snake 后为
78,088 bytes，持续运行后保持在约 78 KiB，没有回落到旧固件约 12.5 KiB 的危险区。

本次同时参考了本地 Pocket Sage 与 xiaozhi-esp32 的 ESP32-S3 defaults。两者共同采用 size optimization、
96–100 KiB internal reserve、PSRAM mbedTLS 和收紧的 Wi-Fi buffer 数量；Pocket Sage 还记录了以下值得保留
的边界：internal reserve 保护大块 DMA/stack 的连续分配，但不增加总内存；`minimum_free` 聚合值在不同
ESP-IDF 版本间不能直接比较；Wi-Fi IRAM/RX IRAM 优化虽然约占数十 KiB DIRAM，却不应在没有吞吐、延迟和
功耗 A/B 时关闭。

BOX-3 当前保持 performance optimization、Wi-Fi IRAM 优化和上游 Wi-Fi buffer 默认值，不为得到漂亮的
静态数字牺牲 compositor 或无线性能。HTTP/3 动态缓冲已经通过其 capability-aware allocator 优先使用
PSRAM。后续只有在真实 Wi-Fi + TLS + audio + Guest 并发压力下，才依次评估以下独立 profile，并记录
internal free、largest block、吞吐、重连、audio underrun 和触控延迟：

1. 收紧 static/dynamic RX、management buffer 和 TCP/IP mailbox；
2. 让 mbedTLS 动态内存优先 PSRAM；
3. 为 internal DMA/驱动分配建立 reserve pool；
4. 对 Host 或选定组件做 `-Os` A/B；
5. 最后才评估关闭 Wi-Fi IRAM/RX IRAM optimization。

这些项目都属于配置与性能策略，不能与普通代码修复捆绑，也不能通过修改生成的 `sdkconfig` 验证。

### RGB565 endian 与色彩正确性

测试图必须覆盖纯红/绿/蓝、低位渐变、灰阶、奇偶 x 坐标、非紧凑 stride、裁剪、alpha 0/1/127/254/255
和 scale 边缘。Host 单元测试使用 canonical memory order；panel 真机测试再验证 transport swap，避免把
红蓝互换或 byte swap 错误误判为 driver controller 差异。

### CPU 和 SPI 争用

320×240 减少像素量，但 CPU compositor、PSRAM、SPI DMA、Wi-Fi 和 audio 仍会争用 cache、DMA channel 和
internal SRAM。任何优化必须用分段 telemetry 证明降低了目标阶段成本；扩大 dirty area、增加全帧 copy 或
把固定 buffer 换成热路径动态分配不算有效优化。

### BSP 硬件变体

display/touch controller、panel byte order 和初始化序列只从固定 BSP 版本及真机 probe 得出。每个被支持的
BOX-3 revision 都要记录板型标识和通过的 controller 组合，但不提交设备序列号、MAC 或个人串口路径。

## 7. 验证与完成定义

计划中的最小自动门禁为：

```sh
# 既有回归
bash tools/build_guest_p4.sh
bash tools/p4.sh build-host
bash tools/s31.sh build-host
bash tools/tests/test_firmware_host.sh
python3 -m unittest tools.tests.test_firmware tools.tests.test_micropixel_cli -v

# ESP32-S3 compile gate 与三款板型 preview
bash tools/s3.sh build-null
bash tools/s3.sh build-host box3
bash tools/s3.sh build-host szpi
bash tools/s3.sh build-host cores3
bash tools/s3.sh build-apps

# 最终三款 S3 Host 与浏览器完整镜像
bash tools/s3.sh build-release box3
bash tools/s3.sh build-release szpi
bash tools/s3.sh build-release cores3
```

BOX-3 preview 完成必须同时满足：

- Xtensa AOT build、load、trap、watchdog 和 shutdown conformance 通过；
- display/touch/audio/Wi-Fi/USB 初始化失败都有明确 unavailable 或错误语义；
- opaque 稳态资源、App Surface、LVGL buffer 和 panel submit 使用 RGB565，未发生整帧 24→16-bit 转换；
- alpha、scale、text 和 streaming texture 在 RGB565 destination 上颜色正确；
- App Hall、Status Layer、系统菜单和 Guest 手势在 320×240 上无越界；
- 16 MiB Flash 分区、8 MiB `app_store`、OTA/Web Console target 和 preview release artifact 通过；
- P4/S31 既有 ABI、Bundle、图形结果和构建门禁无回归；
- 未提交生成物、第三方源码副本、设备标识、原始日志或一次性性能数据。
