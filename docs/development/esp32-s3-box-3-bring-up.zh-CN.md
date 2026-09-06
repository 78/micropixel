# ESP32-S3 preview 适配指南

S3 preview 用来验证同一 Guest 模型在 Xtensa、CPU 图形合成与 SPI 屏幕上的适配边界。
当前维护 BOX-3、立创 SZPI ESP32-S3 和 M5Stack CoreS3；产品基线仍为 P4 + Metalio-Claw4。
本文重点解释 BOX-3 的硬件约束与跨 S3 原则，构建烧录统一见 [烧录指南](flashing.zh-CN.md)。

## 1. 当前能力

BOX-3 已接入 Xtensa AOT、RGB565 图形、320×240 System Shell、触摸、USB 本地控制与 JPEG 截图，
以及原生 Wi-Fi、音频、板载 IMU、Pmod GPIO、OTA 和 Remote Control。历史 P0–P5 任务表不再作为
当前配置或命令入口。

BOX-3 侧边 Mute 归 Host，静音时保持用户保存的主音量。可选能力初始化失败时只标记 unavailable，
不阻断显示、USB 和 Runtime。microSD、麦克风与 Bluetooth 尚未开放 Guest Service；BOX-3 不注册
电池、振动和物理断电能力。外接 SENSOR 板的 AHT30 不是主机内建设备。

## 2. AOT 可移植性

Wasm 和资源可复用，AOT 必须按 CPU 架构分别生成：P4/S31 使用 RISC-V ilp32f，S3 使用 Xtensa。
固定 WAMR fork commit、编译器和 AOT format，不能只凭 wamrc 的版本字符串判断兼容。

当前一个 Bundle 只含一个 AOT section。离线 package 显式选择 target；连接设备时 CLI 自动选择，
CLI 和 Host 都在安装写入前拒绝缺少 target 元数据或架构不匹配的 Bundle。
Host 与 app_store 可以独立烧录，排查异常时必须核对设备实际装载的 AOT，不能只看本地产物。

S3 的 executable PSRAM 缓解 internal SRAM 压力，但没有解除以下限制：

- AOT text 和 relocation 通过 D-bus mirror 写入，从 I-bus alias 执行；写后必须完成指令同步。
- cache 维护只能影响必要的地址范围，不能关闭另一核心正在使用的共享 I-cache。
- Xtensa literal 有寻址距离限制。当前打包策略使用 `--size-level=0` 为大模块生成 literal island；
  不把 relocation 超范围简单归因于 PSRAM。
- Flash/PSRAM 共用 cache 资源。安装和卸载必须经过停止 Guest 的门禁，PSRAM placement 不允许绕过
  BundleFS/Flash 并发限制；其他运行期 Flash 写入需验证 stack、ISR 和 cache 安全。

这些规则属于固定 fork 与构建器的职责，不在板级复制 loader 或修改本机 ESP-IDF 规避问题。

## 3. RGB565 与 CPU 合成

S3 不具备本项目使用的 PPA/DMA2D。像素合成选择软件后端，SPI DMA 只承担面板传输。
不透明内容尽量沿整条路径保持 RGB565，减少内存与重复颜色转换：

```text
资源 → RGB565 texture → App Surface → LVGL draw buffer → panel 字节序适配 → GRAM
```

canonical RGB565 与面板线序分开处理。换字节序仍有成本，但不同于 RGB888→RGB565 转换。
透明资源保留 BGRA8888，直接混合到 RGB565 目标；缩放和旋转避免全尺寸 RGB888 临时帧。
资源可在打包期转为 RGB565，保留的压缩图片在加载时解码。

BOX-3 当前使用 40MHz SPI、40 行 PSRAM double draw buffer，直接从 PSRAM 做 SPI DMA。
双缓冲让 CPU 绘制与传输重叠，减少 flush 次数不一定更快；历史对照中整屏 single buffer 反而失去
这种重叠。80MHz 曾出现显示异常，不能仅凭理论带宽提高默认时钟。
具体参数以 [BOX-3 defaults](../../firmware/espressif/sdkconfig.s3-box-3.defaults)和
[板级实现](../../firmware/espressif/main/platform/boards/esp32-s3-box-3/)为准。

## 4. UI 与能力组合

320×240 使用完整的横屏 Host UI profile，复用页面状态、手势和 Session 生命周期。Guest 按
RendererInfo 布局，逻辑画布为 960×720；应用不根据板名分叉。语义字体由 Host profile 选择，
Guest 不重复缩小字号。

Board 负责引脚、供电、控制器选择和能力注册，复用共享 Wi-Fi、I²C executor、音频 sink 与设备契约。
DisplayTransition 为可选能力，缺失时直接切换 Guest/Hall。截图使用 PSRAM 中的 RGB565 displayed
shadow，在传输换序前更新，以保持 canonical 像素。

所有 LVGL 对象、flush 与 event callback 必须在 worker 启动前完成注册，运行期修改需持正确锁。
否则冷启动 refresh 与初始化可能并发修改非线程安全的 allocator，造成难以复现的内存破坏。

## 5. 内存预算

芯片标称 SRAM 不等于可分配的 internal heap：IRAM、静态数据、cache 与驱动先占用一部分。
诊断同时看空闲总量、最大连续块和运行高水位，不能仅比较一个 heap 数字。

图形大缓冲、displayed shadow、截图、LVGL object pool 和 relocated AOT text 使用 PSRAM。
只有满足 cache 安全条件的任务栈才能放入 PSRAM；Guest 主线程和硬件明确要求的 DMA 资源仍需
internal memory。能力分配失败应明确降级，不静默挤占保留给关键路径的内部空间。

调整 Wi-Fi buffer、编译优化或 IRAM 策略必须单独 A/B，记录吞吐、重连、音频 underrun 和触控延迟。
不要为增加空闲内存数字牺牲可持续性能，也不要通过修改生成的 sdkconfig 验证长期配置。

## 6. 测量与验收

按四层测量，区分峰值、持续吞吐和真实 App 表现：

| 层 | 场景 | 关键指标 |
|---|---|---|
| Panel transport | 整屏、不同高度 strip、多脏区、single/double buffer | bytes、DMA/等待、flush-ready、错色与撕裂 |
| CPU compositor | fill、opaque copy、alpha、scale、text | 固定成本、ns/pixel、PSRAM 带宽 |
| Guest Scene | Snake 小 damage、Blocks、粒子、滚屏、streaming update | submit、可见 refresh 和 panel flush 的不同速率 |
| 并发 | 音频、Wi-Fi、解码、截图、暂停恢复 | P95/P99、触控延迟、underrun、heap、watchdog |

能力测量不以 60Hz Timer 限制提交速度，也不把高提交率当作屏幕 FPS。可持续验收至少运行 10 分钟，
要求无画面损坏、爆音、watchdog 或持续内存下降。Bundle 安装另按停止 Guest 的正常流程测试，不能
为了并发压力绕过安装门禁。分段方法见 [Graphics 性能诊断](graphics-performance.zh-CN.md)。

颜色验证覆盖纯色、灰阶、非紧凑 stride、裁剪、alpha 边界和缩放；UI 验证覆盖触摸边缘、长标题、
键盘、最大 App 数量与 Guest/Hall 切换。AOT 验证覆盖大模块、trap、watchdog、清理和反复冷启动。

自动检查包括相关 Host test、CLI test、Guest 构建与三款 S3 Host 构建；涉及共享图形时同时构建 P4/S31。
发布通过 `tools/s3.sh build-release BOARD` 生成对应板型产物，BOARD 为 box3、szpi 或 cores3。
OTA rollback、BundleFS 断电恢复与生产分发仍按跨板发布门槛验收，不因 preview 接入而视为完成。

## 7. 硬件来源与变体

BOX-3 的基础配置为 16 MiB Flash、16 MiB PSRAM 和 320×240 触摸屏。不同批次可能使用不同 display/touch
controller，维护时以固定版本 BSP 和真机 probe 为准，不假定所有 BOX-3 相同，也不将某块板的验证外推
到其他 S3 板型。板级可开放 GPIO 以实际白名单为准，不能占用 USB 或系统总线。

硬件资料沿用官方来源，不在仓库分发副本：

- [ESP-BOX-3 BSP](https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/README.md)
- [BSP pin/capability header](https://github.com/espressif/esp-bsp/blob/master/bsp/esp-box-3/include/bsp/esp-box-3.h)
- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESP-IDF external RAM 约束](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html)
