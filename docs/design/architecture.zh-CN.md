# MicroPixel 架构与发布基线

本文只记录当前产品架构、稳定边界和尚未完成的发布门槛。已被代码取代的原型、候选 API、
历史里程碑和一次性性能实验不在仓库中继续维护。

具体公开接口以 [Guest C++ SDK](../../guest/sdk/README.md)为准，机器可读协议以
[`micropixel_abi.h`](../../guest/abi/micropixel_abi.h) 和 [Guest–Host ABI](../../guest/abi/README.md)为准，
Firmware 文件职责以 [Espressif main README](../../firmware/espressif/main/README.md)为准。

## 1. 产品边界

MicroPixel 是面向嵌入式设备的 WebAssembly 应用运行时。当前产品基线为：

- Host：ESP32-P4 + Metalio-Claw4，ESP-IDF 6.1；
- Runtime：MicroPixel WAMR fork 固定 commit、AOT format v6，同时最多运行一个 Guest `AppSession`；
- Guest：受限 C++23 profile，不直接依赖 ESP-IDF、LVGL 或板级 SDK；
- Guest 内存：Wasm linear memory 位于 PSRAM、按 64 KiB page 增长并由 Host 硬限制；P4 当前单 Guest
  策略上限为 8 MiB，实例化时按最大连续 PSRAM 块自适应下调并保留 Host 安全余量；Host-owned
  Bitmap/offscreen surface 另有 12 MiB 配额；
- App 分发：Bundle v1 封装 AOT、资源和 App metadata；24 MiB 可写 App Store 由 BundleFS 以离散
  64 KiB 块和四 Bank Catalog 提供写时复制事务；
- 系统 UI：Host 原生 App Hall、Status Layer 和系统手势，不作为 Guest App 运行。

当前不承诺 ESP32-S3 产品适配、多 Guest 并行、Guest 多线程、生产级在线 App 分发、网络 Service、
Camera Service 或通用 Widget Server。Remote Control 的开发版在线安装已经进入代码基线，但 package
数字签名、BundleFS 真机断电恢复矩阵和完整 TLS peer 验证仍是发布门槛。BundleFS 的块号表允许物理块
离散分布，卸载后的块可直接复用，不依赖连续 extent GC。

## 2. Host 分层

```text
app_main
    └── FirmwareApp                         # 唯一组合根
        ├── Platform
        │   ├── Metalio-Claw4                # display/input/audio/sensors/GPIO/system UI
        │   └── Null                          # 硬件无关编译基线
        ├── DeviceServices
        │   ├── Graphics / Input / Audio / Random
        │   └── Devices / Sensors / GPIO / Haptics / PowerInfo
        ├── AppRuntime                            # 长驻 WAMR
        │   └── AppSession (0..1)
        │       ├── Bundle / module / instance / exec env
        │       ├── GuestContext / Service endpoints
        │       ├── EventQueue / Timer / Storage / Resource
        │       └── ABI adapter
        ├── HostController / SystemShell
        │   ├── App Hall
        │   ├── Foreground / Suspended
        │   └── Status Layer / System gestures
        └── Conformance hooks                    # 仅测试配置
```

依赖方向固定为：

```text
Runtime -> Device contracts <- Platform
```

- `device/` 定义硬件无关能力契约；
- `platform/` 实现板级 backend，不 include Runtime；
- Runtime 只使用注入的 `DeviceServices`，不 include Platform；
- `FirmwareApp` 是唯一同时知道 Platform、Device 和 Runtime 的组合根；
- `runtime/abi/` 只做 Guest 内存验证、wire 转换、上下文查找和转发，不承担业务规则。

Platform 内部按职责分层，依赖只能向下：

```text
boards/<board>  ->  common + drivers + lvgl + radio + transports
       │                         │
       │                         └─ uses SoC-selected ESP-IDF capabilities
       └─ owns pin mapping, power sequence, peripheral composition and board metadata
```

- `platform/common/`：可跨板复用的 ESP32 backend 与 contract adapter；
- `platform/drivers/`：按器件型号组织的驱动，不知道开发板；
- `platform/lvgl/`：显示合成、Guest renderer、字体与按分辨率组织的 Host UI profile；
- `platform/radio/`：Hosted 与 SoC-native Wi-Fi radio strategy；
- `platform/transports/`：USB Serial/JTAG 等本地控制字节传输；
- `platform/soc/<target>/`：只选择 SoC component 依赖，不组合开发板；
- `platform/boards/<board>/`：板型组合根，只拥有 wiring、启动/关机次序和不能复用的板载外设。

板型由 `MICROPIXEL_BOARD` Kconfig choice 唯一选择。新增板型的正常变更面是新的
`boards/<board>/`、一个 choice symbol、Platform source selection 与产品 defaults；Guest ABI、Runtime 和
HostController 不因板名变化。系统信息和 Remote Control 的硬件描述统一读取
`device::HardwareInfoBackend`。`bash tools/p4.sh build-null` 和 `bash tools/s31.sh build-null` 是各 SoC 的
硬件无关依赖方向编译门禁，不生成可烧录的产品镜像。ESP-Mosaico 的 P0/P1 profile 已接入 Runtime、
BundleFS、native Wi-Fi、板级供电、官方 CO5300 显示和 CST9217 中断触摸，并复用 App Hall、Status Layer、
逻辑 viewport、分辨率 layout profile、系统转场时间线及 PPA/DMA2D 图形原语。P4 的 RGB888 framebuffer
提交与 S31 的 RGB565/QSPI 提交留在各自 display pipeline；缩放、位图复制和颜色转换不回退为正常帧路径的
CPU 整图逐像素循环。音频和板载传感器在权威 BSP 路径完成集成与真机验证前显式 unavailable，NAND
App Store 与扩展模块属于 P2。

## 3. Session 与事件模型

`AppRuntime` 与 Firmware 同寿命，WAMR 只初始化一次。每次启动 Guest 创建一个 `AppSession`，
并按 Bundle mapping → module → instance → exec env → `GuestContext` 的所有权链管理。正常返回、Trap、
启动失败和应用切换使用同一套逆序清理。

Guest 为单线程事件模型，标准入口只有：

```cpp
micropixel::Application app;
micropixel::Timer timer = app.timers().Every(16_ms);

app.Run([&](const micropixel::Event& event) {
    if (const micropixel::TimerEvent* tick = event.TimerFrom(timer)) {
        Update(*tick);
        Render();
    } else if (const micropixel::TouchEvent* touch = event.touch()) {
        HandleTouch(*touch);
    } else if (event.type() == micropixel::EventType::kResume) {
        Render();
    } else if (event.type() == micropixel::EventType::kStop) {
        SaveState();
    }
});
```

- handler 串行执行，不会在一个 handler 中间派发下一个事件；
- `Stop` 先交给 handler，handler 返回后 `Run()` 返回；
- Timer 只由 `app.timers().After/Every()` 创建，并作为普通 Event 匹配；
- `WaitEvent()`、`WaitEventFor()` 和 `PollEvent()` 只保留给短期等待和协议测试，不是长期 App 的第二套
  标准写法；
- Host 暂停 Guest 时通过 Runtime 内部控制消息等待 `event_wait` 安全点，并同时暂停 watchdog；不存在 Guest
  可见的 `Pause` 事件。恢复同一 Session 时只投递 `Resume`；
- 系统手势由 Host 拦截，不泄漏给 Guest。

电源管理由 Host supervisor 独立持有 `Awake / EnteringSleep / Asleep / Waking / ShuttingDown` 状态，不与
`Hall / Foreground` 或 App lifecycle 混成一个枚举。单击电源键时，前台 App 先完成上述安全点确认，再渐暗
背光并进入平台 light sleep；唤醒后先恢复显示硬件和原 Session，再渐亮。安全点等待超过 500ms 时强制停止
App，唤醒目标为 App Hall。该流程不修改 Guest ABI，也不靠“再次调用 wait”作为确认。
进入低功耗的过渡期内，电源键驱动会保留首个请求对应的按下代次；若再次检测到物理按下，则在真正执行
light sleep 前取消本次入睡、恢复显示。Host 状态机不是 `Awake` 时也不会接受新的入睡请求，唤醒按键释放
产生的延迟 click 由短暂 guard 窗口过滤。若按键唤醒后仍保持按下，则必须先观察到物理 `PRESS_UP` 才重新
开放电源请求，避免长按唤醒过程中再次进入休眠。真正调用 light sleep 前会重新读取 TCA9555 输入并确认
共享中断线已经释放；若 ESP-IDF 因瞬态唤醒条件拒绝入睡，则保持显示关闭、清除条件后原地重试，不把拒绝
误当作一次成功唤醒。

亮屏且电源状态为 `Awake` 时，新一轮按下持续 2 秒进入终态 `ShuttingDown`；唤醒所用的同一轮长按不会
触发关机。Host 先停止当前 App、取消远控输入并静音，再显示 `Shutting down...` 系统画面并立即开始物理
关机；Metalio-Claw4 平台把 TCA9555 `P0.4 / PWR_KEY_PULSE` 配置为输出，以 100ms 高、100ms 低持续脉冲，
画面在脉冲期间保持显示，直到电源管理芯片切断电源。OTA 正在写入时拒绝休眠和关机，避免中断固件事务。

System Settings 的 Power Management 页面提供 Off 或 1/5/10/30 分钟的自动休眠策略。它是 Host
supervisor 的空闲 deadline，不是另一套平台电源状态机：仅在外接电源状态明确为未连接时生效，任意触摸或
语义按键活动刷新 deadline，插电暂停、拔电和唤醒重新计时；到期后仍提交与短按电源键相同的请求。因此 App
安全点、显示释放/恢复、电源键唤醒、OTA 排斥和失败恢复语义保持单一来源。

## 4. Guest–Host 边界

Public C++ SDK 不直接暴露 C ABI。`guest/runtime/sdk.cpp` 将强类型对象 lower 到七个 Core imports：

1. `abi_version`
2. `log_write`
3. `clock_now`
4. `event_wait`
5. `service_open`
6. `service_call`
7. `service_submit`

能力通过带独立 major/minor 的 Service 演进：

| Service | ID | 当前传输 |
|---|---:|---|
| Timer | 1 | call + event |
| Storage | 2 | call |
| Resource | 3 | call |
| Random | 4 | call |
| System | 5 | call |
| Devices | 6 | call；为热插拔保留 event |
| Graphics | 16 | call + submit |
| Input | 17 | call + event |
| Audio | 18 | call + event |
| Network | 19 | 仅预留，未实现 |
| Sensors | 20 | call |
| GPIO | 21 | call + event |
| Haptics | 22 | call + event |
| PowerInfo | 23 | call |

三条数据路径的职责不混用：

- `service_call`：低频、有界的控制请求和响应；
- `service_submit`：Renderer Frame command stream 等高频或批量数据；
- `event_wait`：Timer、Touch、Audio playback、Resume 和 Stop 等异步通知。

`micropixel_event_t` 固定为 48 bytes，按 `service_id + event_id` 解码。Service descriptor、handle 和
设备信息在 Guest 生命周期内缓存；Host 热路径使用固定容量 Registry，不做字符串查找或堆分配。

版本兼容规则为：Service major 必须相同，Host minor 必须不低于 Guest 的最低要求。已发布的
Service、method、channel、event、capability 和 opcode ID 不得改义或复用。新能力优先增加
method/channel/event，其次增加 Service，只有真机证据证明现有传输不足时才增加 Core import。

### 4.1 设备发现与组合设备

Devices Service 是类似操作系统设备目录的发现面，但不把 Linux device tree、ACPI、ESP-IDF driver 或
板级地址暴露给 Guest。Platform 构造固定容量 catalog；应用枚举 opaque `DeviceId`，读取 kind、parent、
capabilities 和显示名称，再把同一个 ID 交给 Sensors、GPIO、Haptics、PowerInfo 等能力 Service。

`DeviceId` 与枚举 index 分离。parent 支持组合设备：未来外接两个手柄时，每个手柄有独立身份，手柄内的
传感器是带 parent 的独立 device；Camera、Location 和更多 SensorKind 可以扩展 catalog 与对应 Service，
不修改现有应用的枚举流程。第一阶段 catalog 是 Session 内静态快照，但 ABI 已保留 generation 与
added/removed event。

Metalio-Claw4 第一阶段暴露内置加速度计、磁力计、震动马达、电源信息和 14 根板上确认可开放的 GPIO。
GPIO 不要求出厂 binding：每根引脚以物理 line number 和 capability 枚举，应用选择任意一根后直接打开。
打开形成 Session 内独占 lease；关闭或 Session teardown 后恢复 input/无上下拉安全状态。板级已占用引脚
根本不进入 catalog。

Sensors 使用 `Sensor<Reading>` typed resource；Acceleration 和 MagneticField 有各自单位与 value type，
温度、光照、压力等以后增加独立 reading type，而不是扩张一个万能对象。没有 sensor handle 时芯片保持
suspend，也没有专属 sensor task；第一个 `Open` 只向板级共享 I²C executor 注册周期采样，最后一个 handle
释放后注销采样并让芯片休眠。Platform 固定槽位维护最新值缓存，Guest `Read` 自主读取缓存，不产生 Sensor
event。`Open` 表示 App 正在使用传感器，默认采样为游戏可用的 100 Hz；Guest 可在 `SensorInfo` 宣告范围
内配置采样间隔，Host 将硬件 ODR 映射到不慢于请求频率的档位。低功耗边界是无 handle 或 App Suspend，
不是已打开的传感器。

Metalio-Claw4 的一条物理 I²C bus 只保留一个 4 KiB `micropixel_i2c` executor。它使用固定容量的
high/normal/low 队列：GT911 和电源控制优先，Sensor 周期采样与电池刷新可合并且处于低优先级。Touch ISR
和 Sensor timer callback 只做无阻塞投递，实际总线事务都在 executor task 中串行执行；同步调用者提交固定
槽位请求并等待完成。音频实时 I²S mixer 和 GPIO 不属于 I²C executor，仍保持各自的实时/中断边界。

GPIO 主动 `Read`、output 和 PWM 不需要 Host worker。只有第一个订阅 rising/falling/both edge 的 input
`Open` 才创建 4 KiB `micropixel_gpio` bridge；ISR 只把固定 POD 写入有界队列，worker 在任务上下文转换为
Guest event。最后一个 edge input 释放或 App Suspend 时卸载对应 ISR handler 并停止 worker；Resume 只在仍
有 edge handle 时恢复。该 worker 不轮询，也不与 I²C executor 合并。

## 5. Graphics 与 Resource

公开 Graphics 使用 SDL3 风格概念：

```text
Renderer -> Frame -> Present
Resources -> Texture
Renderer -> StreamingTexture / TextureUpdateBatch
```

- `Frame` 直接提供 `Clear`、`FillRect`、`DrawText`、`DrawTexture` 和
  `Save/SetClipRect/Translate/Restore`；
- `Point`、`Rect` 和 `Size` 是 Renderer 与 Input 共用的逻辑坐标 value；显示尺寸只由 `RendererInfo`
  公开，`InputInfo` 不维护第二份尺寸来源；
- 一个 Frame 是一次原子场景替换，`Present()` 显式发布；析构不会隐式 Present；
- SDK 将大 Frame 自动分成有界 batch，应用不感知 transport 或 retained 优化开关；
- 公开资源概念统一为 `Texture`；Host 内部的 `BitmapView` 只是 CPU 像素内存描述符，
  不是公开资源身份；
- `Resources::LoadTexture(AssetId)` 同步返回 `Result<Texture>`，Host 可在内部使用 worker 解码；
- `Texture` 和 `StreamingTexture` 是 move-only RAII 对象，`Reset()` 立即令 Guest 句柄无效；
- retained scene 持有独立 Texture 引用。Guest `Reset()` 只撤销 Guest 引用，当 scene 引用也归零时
  才释放像素内存；
- `PixelFormat::kBgr888` 和 `kBgra8888` 按 Guest 内存中的字节顺序命名；
- `StreamingTexture::Update()` 按 dirty rect 更新，`TextureUpdateBatch` 合并 damage 并在
  `Finish()` 时统一唤醒 compositor。

## 6. 所有权、并发与错误

- Firmware 的 WAMR、Bundle mapping、NVS、timer、queue、task 和板级句柄使用 move-only RAII
  或 scope-bound binding 管理；
- 裸指针默认 non-owning；跨异步边界必须由 binding token、join 或 shutdown protocol
  证明生命周期；
- ISR 只记录最小 POD 状态并唤醒任务，不调用 WAMR、Guest 或 LVGL；
- 跨任务状态使用固定容量队列、数组和对象池，实时路径不隐式扩容；
- App Hall 封面与 Guest 压缩图片解码共享一个固定容量、低优先级后台执行器；watchdog 复用 ESP Timer
  task，通过单调时钟 deadline、阻塞停止和回调状态检查保证续期与销毁边界，不为每次 Guest 调用创建线程；
- shutdown 顺序是停止接收、唤醒 worker、join，再释放队列和底层句柄；
- Guest 可恢复失败返回 `Result<T>`；无效句柄、SDK/Host 自相矛盾或 ABI 安全失败进入
  panic/fault policy；
- 析构函数只做 best-effort cleanup，不 Panic、不抛异常；
- exception 和 RTTI 保持关闭，Public SDK 不暴露 STL ABI、Host 指针或 C++ 对象布局。

## 7. Bundle、能力与权限

Bundle v1 当前使用 128-byte header 和 48-byte section，section 可包含 AOT、Asset 和 App
metadata。Bundle reader 在创建 WAMR instance 前完成 magic、hash、范围、对齐、格式和唯一性检查。
Bundle 文件由 [BundleFS](bundlefs.zh-CN.md) 保存；Catalog 位于 `app_store` 自身，不使用 NVS，也没有
预置 App 或连续 extent 的特殊语义。

设备能力与 App 权限必须分开建模：

- Service requirements 回答设备是否提供某 Service、版本和语义能力；
- Permission grant 回答当前 App 是否获准执行某个受保护操作；
- 缺少 Service 应报告 `NotFound/Unsupported/VersionMismatch`，未授权应报告
  `PermissionDenied`；
- `service_open` 只做 Service 发现和版本协商，不承担用户授权流程；
- 权限应按动作划分，例如 `storage.read` 和 `storage.write`，不用过宽的 Service 级布尔值。

当前 Bundle 尚没有 requirements section，权限声明和 grant 也未实现；这两项不得用
Guest 进入 `main()` 后的 Trap 代替。

## 8. 发布基线

当前集成应用为 Blocks、Snake 和 Demo。Demo 覆盖公开 Service，Snake 覆盖高频 Graphics、
Input、Storage 和 Audio，Blocks 覆盖 StreamingTexture 与批量 damage。

自动基线：

```sh
bash tools/build_guest_p4.sh
bash tools/p4.sh build-host
bash tools/p4.sh build-all
bash tools/check_firmware_style.sh
bash tools/tests/test_firmware_host.sh
python3 -m unittest tools.tests.test_build_app_store_image
```

真机发布前至少检查：

- App Hall 能显示并启动最多 50 个 App，第一行可自由左右拖动并带惯性、不强制卡片吸附；新安装 App 位于
  最左侧。卡片和解码封面使用最多 6 项的视口窗口，快速滑动期间允许占位；切换时旧 Session 已先销毁；
- App Hall 顶部居中显示紧凑 FPS/CPU，右侧 Wi-Fi 信号与电池状态不会互相遮挡；
- 顶部下滑打开 Status Layer，底部上滑暂停并返回大厅；
- 恢复同一 App 时复用 Session 和 retained scene，并由 PPA 从 Hall 卡片硬件放大回最后一帧；
- Texture `Present → Reset → 重绘/换场景` 不产生 UAF；
- Timer 积压时 `elapsed` 和 `missed_count` 正确；
- GT911 不声明 pressure capability，Touch 坐标与 Renderer 使用同一逻辑空间；
- 亮度、Host 主音量、FPS/CPU 蒙层、音频结束状态和设备重启后的随机源正常。
- 电源键从前台 App 和大厅均可进入 light sleep；前台 App 正常路径唤醒后只收到一次 `Resume`，安全点超时
  路径停止 App 并回到大厅，唤醒按键释放产生的 click 不会立即再次休眠；
- 亮屏时新按下并持续 2 秒会先停止 App、显示关机画面，再通过 `PWR_KEY_PULSE` 实际切断电源；关机画面
  利用脉冲执行时间保持可见，不额外固定等待；
  唤醒后持续按住的同一轮按键不会误触发关机，OTA 写入期的关机请求会被拒绝；

尚未完成的发布门槛只保留下列当前事项：

1. Bundle requirements 与 WAMR instance 创建前的兼容性 preflight；
2. 权限声明、grant 与 method 级检查；
3. Resource/Graphics/Input 协议版本冻结、兼容 fixture 和 wire 负向/fuzz 回归；
4. Texture retained 生命周期、Timer 积压、Run/Stop 和三个应用的真机回归；
5. System Shell 的在线安装/卸载、网络配置和完整错误恢复。

## 9. 架构禁止项

- 不引入深继承树、万能 Manager、Service Locator 或新的全局可变状态；
- 不让 `FirmwareApp`、ABI adapter 或 Platform 承担本属于领域 Service 的业务规则；
- 不在 C ABI 中暴露 C++ class、`std::expected`、vtable、STL 类型或 Host 指针；
- 不用裸 `new/delete`、无界容器或 detached task 承担实时资源与生命周期；
- 不为新板型分叉 Guest API；板级差异通过 Device contract、Service capability 和 Bundle
  requirements 表达；
- 不修改第三方源码以迎合本项目格式、命名或文档结构。
