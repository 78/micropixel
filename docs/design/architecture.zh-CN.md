# MicroPixel 架构与发布基线

本文只记录当前产品架构、稳定边界和尚未完成的发布门槛。已被代码取代的原型、候选 API、
历史里程碑和一次性性能实验不在仓库中继续维护。

具体公开接口以 [Guest C++ SDK](../../guest/sdk/README.md)为准，机器可读协议以
[`micropixel_abi.h`](../../guest/abi/micropixel_abi.h) 和 [Guest–Host ABI](../../guest/abi/README.md)为准，
Firmware 文件职责以 [Espressif main README](../../firmware/espressif/main/README.md)为准。

## 1. 产品边界

MicroPixel 是面向嵌入式设备的 WebAssembly 应用运行时。当前产品基线为：

- Host：ESP32-P4 + Metalio-Claw4 是产品 profile；ESP32-S31 + ESP-Mosaico、ESP32-S3-BOX-3、立创
  SZPI ESP32-S3 与 M5Stack CoreS3 是同步维护的 preview profile；三个芯片 target 使用 ESP-IDF 6.1；
- Runtime：MicroPixel WAMR fork 固定 commit、AOT format v6，同时最多运行一个 Guest `AppSession`；
- Guest：受限 C++23 profile，不直接依赖 ESP-IDF、LVGL 或板级 SDK；
- Guest 内存：Wasm linear memory 位于 PSRAM、按 64 KiB page 增长，P4、S31 与 S3 的当前策略上限均为
  8 MiB；实例化时按最大连续 PSRAM 块自适应下调，后续增长也必须保留 Host 安全水位。Host-owned
  Bitmap/offscreen surface 不预留累计配额，而是在每次实际分配时使用同一安全水位做动态准入；
- App 分发：Bundle v1 封装 AOT、资源和 App metadata；P4 的 24 MiB 与 S31/S3 的 8 MiB 可写 App Store
  都由 BundleFS v2 以离散 64 KiB 块和四个 16 KiB Catalog Bank 提供写时复制事务；
- 系统 UI：Host 原生 App Hall、Status Layer 和系统手势，不作为 Guest App 运行。

当前不承诺把 ESP32-S3 preview 提升为产品 profile、多 Guest 并行、Guest 多线程、生产级在线 App 分发、网络 Service、
Camera Service 或通用 Widget Server。Remote Control 的开发版在线安装已经进入代码基线，但 package
数字签名、BundleFS 真机断电恢复矩阵和 TLS 负向真机矩阵仍是发布门槛。BundleFS 的块号表允许物理块
离散分布，卸载后的块可直接复用，不依赖连续 extent GC。

## 2. Host 分层

```text
app_main
    └── FirmwareApp                         # 唯一组合根
        ├── Platform
        │   ├── Metalio-Claw4                # ESP32-P4 product board
        │   ├── ESP-Mosaico                  # ESP32-S31 preview board
        │   ├── ESP32-S3-BOX-3               # ESP32-S3 preview board
        │   ├── SZPI ESP32-S3                # 立创开发板 preview
        │   ├── M5Stack CoreS3                # ESP32-S3 preview board
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

- `device/contracts/` 定义硬件无关能力契约，`device/` 根部提供 Runtime 使用的 façade；
- `platform/` 实现硬件契约并装配 Board，不 include Runtime；
- Runtime 只使用注入的 `DeviceServices`，不 include Platform；
- `FirmwareApp` 是唯一同时知道 Platform、Device 和 Runtime 的组合根；
- `runtime/abi/` 只做 Guest 内存验证、wire 转换、上下文查找和转发，不承担业务规则。

Platform 内部按职责分层，依赖只能向下：

```text
boards/<board>  ->  audio + haptics + wifi + adapters + buses + drivers + lvgl + transports
       │                                           │
       │                                           └─ uses target-selected ESP-IDF capabilities
       └─ owns pin mapping, startup order, peripherals, controllers and board metadata
```

- `platform/adapters/`：把具体呈现操作适配成硬件无关契约的窄 adapter；
- `platform/audio/`、`platform/haptics/`、`platform/wifi/`：按领域放跨板复用的引擎、Peripheral 和管理器；
- `platform/random/`、`platform/defaults/`：通用随机源和缺失能力的默认实现；
- `platform/controllers/`：跨板复用的板级控制算法；
- `platform/buses/`：共享物理总线的串行调度与优先级执行器；
- `platform/drivers/`：按器件型号组织的驱动，不知道开发板；
- `platform/lvgl/`：LVGL 与平台的共享桥接，只包含显示合成、Guest renderer、字体与 Host 指针路由；
- `host/ui/lvgl/`：Host 拥有的 LVGL App Hall、Status Layer、系统页面、共享 UI 状态与分辨率 layout profile；
- `platform/transports/`：USB Serial/JTAG 等本地控制字节传输；
- `platform/targets/<target>/`：只选择 SoC component 依赖，不组合开发板；
- `platform/boards/<board>/`：板型组合根，只拥有 wiring、启动/关机次序、不能复用的板载外设，以及
  初始化成功后的 `BoardRegistration` 声明。`platform.cpp` 保持为窄组合入口；私有 `platform_state.hpp`
  只组合一个 `SquareSystemUiState`，不再逐项拥有 LVGL 页面、Hall 数组、指针路由或主题状态。Hall 卡片由
  `host/ui/lvgl/` 统一按有界窗口虚拟化；板型 `presentation.*` 只提供 framebuffer/GRAM 所有权，并按需实现
  `DisplayTransition`、`ScreenCapture`、`BrightnessControl`、`VolumeControl` 和 `ShutdownPresentation`，
  不定义页面、Hall 生命周期或产品 UI 属性。

`FirmwareApp` 只读取 `PlatformServices`，不调用板型 getter，也不根据板名分支。Board 只提交已经完成初始化的
`BoardRegistration`；`Platform::Publish()` 统一补齐 unavailable 实现、公共 `AudioEngine` 与 `DeviceRegistry`，
因此服务集合完整性和硬件支持程度是两个独立概念。`NullBoard` 只提交 `BoardInfo`，用于证明新增板型可以从
最小声明开始，再逐步增加显示、输入、音频和 Peripheral。

顶部 Status Layer 由 `host/ui/lvgl/square_common/status_layer_ui.*` 唯一实现，以 ESP-Mosaico 的
480×480 控件结构和交互语义为基准，720×720 profile 只调整几何与字体。快捷项、亮度和音量全部使用 LVGL
原生 button/slider/bar 与 event callback；板级不得重新实现 hit-test 或控件状态，只提供转场以及亮度、
音量的能力落地。

`host/ui/lvgl/square_common/square_ui_state.*` 与 `square_system_ui.*` 是方屏 Host UI 的共享状态与
生命周期边界，集中拥有主题安装、启动/加载/关机页、Guest 前台层级、App Hall、系统页面、Status Layer、
手势和 Host 指针路由。
`virtualized_hall_policy.*` 只物化可见卡片及前后预取窗口，使 UI 对象和解码封面的内存占用不随已安装应用
数量线性增长；480/720 profile 分别完整声明该分辨率的页面几何、字体角色、启动呈现和转场参数，开发板只
选择其中一个完整 profile。共享 System UI 把 Hall 卡片目标区域和封面参数收敛为转场请求；板级接口只消费
该请求并操作 framebuffer/GRAM，不读取 Hall 索引、滚动位置或 Host UI 对象。这些产品属性和生命周期不得
散落回具体开发板实现。

Host LVGL 实现统一使用 `micropixel::host_ui::lvgl::square_common` namespace。共享 `SquareSystemUi` 实现
`host_ui::SystemUi` 契约，并只通过 `SquarePresentation` 的窄接口访问板级呈现。芯片能够加速 Guest↔Hall
转场时实现完整 `DisplayTransition`；不具备该能力时返回空接口，共享 Host UI 仍可工作。屏幕抓取、亮度、
音量和关机呈现同样是独立可选接口，Board 不反向拥有 Host UI 页面。

板型由 `MICROPIXEL_BOARD` Kconfig choice 唯一选择。新增板型的正常变更面是新的
`boards/<board>/`、一个 choice symbol、Platform source selection 与产品 defaults；Guest ABI、Runtime 和
HostController 不因板名变化。系统信息和 Remote Control 的板型描述统一读取 `device::BoardInfo`。
`bash tools/p4.sh build-null` 和 `bash tools/s31.sh build-null` 是各 SoC 的
硬件无关依赖方向编译门禁，不生成可烧录的产品镜像。ESP-Mosaico 的 P0/P1 profile 已接入 Runtime、
BundleFS、native Wi-Fi、板级供电、CO5300 显示和 `78/esp_lcd_touch_cst92xx` 中断触摸组件，并复用 App Hall、Status Layer、
ES8311/NS4150B 音频、BQ27220 电池与数字振动电机，并复用共享固定容量音频引擎、
逻辑坐标变换、分辨率 layout profile、系统转场时间线及 PPA/DMA2D 图形原语。P4 的 RGB888 framebuffer
提交与 S31 的 RGB565/QSPI 提交留在各自 display pipeline；缩放、位图复制和颜色转换不回退为正常帧路径的
CPU 整图逐像素循环。BMI270 与双 BMM150 通过固定版本 Bosch SensorAPI 接入同一块板级 I²C executor，
POWER/Function Button、状态 LED、白名单扩展 GPIO、主动电池刷新与 SAM8108 light-sleep/关机路径也由
Platform 提供。Function Button 归一化为 Confirm key；GPIO3 橙色单色状态 LED 通过现有 GPIO Service
暴露为只输出逻辑设备，`true` 表示点亮并由 Platform 处理低有效。三颗传感器的板坐标轴映射与磁场校准
仍需真机验收。NAND App Store 与扩展模块属于 P2。

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
- 系统手势由 Host 拦截，不泄漏给 Guest。顶部下滑使用整条顶部边缘；底部上滑只从屏幕中央三分之一区域
  开始识别，左右两侧输入继续传给 Guest。Host 在 App 启动或恢复后的 3 秒显示底部手势提示条。

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
- `service_submit`：Graphics Scene keyframe/patch 等高频或批量数据；
- `event_wait`：Timer、Touch、Audio playback、Resume 和 Stop 等异步通知。

`micropixel_event_t` 固定为 48 bytes，按 `service_id + event_id` 解码。Service descriptor、handle 和
设备信息在 Guest 生命周期内缓存；Host 热路径使用固定容量 Registry，不做字符串查找或堆分配。

版本兼容规则为：Service major 必须相同，Host minor 必须不低于 Guest 的最低要求。已发布的
Service、method、channel、event、capability 和 opcode ID 不得改义或复用。新能力优先增加
method/channel/event，其次增加 Service，只有真机证据证明现有传输不足时才增加 Core import。

### 4.1 设备发现与组合设备

Devices Service 是类似操作系统设备目录的发现面，但不把 Linux device tree、ACPI、ESP-IDF driver 或
板级地址暴露给 Guest。Board 只向 `BoardRegistration` 登记“Peripheral + Peripheral 内部 Channel + 物理显示名称”；
上层 `DeviceRegistry` 决定 kind/capability、构造固定容量 catalog 并分配 opaque `DeviceId`。应用读取 kind、
parent、capabilities 和显示名称，再把同一个 ID 交给 Sensors、GPIO、Haptics、PowerInfo 等能力 Service。

`DeviceId` 与枚举 index 分离。parent 支持组合设备：未来外接两个手柄时，每个手柄有独立身份，手柄内的
传感器是带 parent 的独立 device；Camera、Location 和更多 SensorKind 可以扩展 catalog 与对应 Service，
不修改现有应用的枚举流程。第一阶段 catalog 是 Session 内静态快照，但 ABI 已保留 generation 与
added/removed event。

Metalio-Claw4 第一阶段暴露内置加速度计、磁力计、震动马达、电源信息和 14 根板上确认可开放的 GPIO。
GPIO 不要求出厂 binding：每根引脚以物理 line number 和 capability 枚举，应用选择任意一根后直接打开。
厂商可按说明书使用 `P15`、`GPIO15` 等物理名称；该名称只用于人机识别，不是公开 `DeviceId`，也不参与路由。
打开形成 Session 内独占 lease；关闭或 Session teardown 后恢复 input/无上下拉安全状态。板级已占用引脚
根本不进入 catalog。

Sensors 使用 `Sensor<Reading>` typed resource；Acceleration、AngularVelocity 和 MagneticField 有各自单位与 value type，
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

公开 Graphics 使用 retained scene graph：

```text
Renderer -> Scene(root) -> ContainerNode -> ContainerNode / Sprite / SpriteBatch / Shape / Label / SurfaceNode
Resources -> Texture / Font
Renderer -> StreamingTexture / TextureUpdateBatch -> SurfaceNode
```

- 一个 Guest 同时只有一个 Active Scene。Scene 是隐式根，`ContainerNode` 可任意嵌套，用于页面、面板、
  对话框、世界/HUD 分组以及子树的 clip、opacity、visibility、z-order 和整体 translation；
- `Scene` 与 `ContainerNode` 共同实现 Public SDK 的 `Container` 创建接口。创建调用的 receiver 就是 parent：
  `scene.CreateContainer()`、`page.CreateContainer()`、`dialog.CreateLabel()`；不保留把 parent 作为参数的第二套
  Create API。组合控件也使用 `page.CreateTextButton()`。点语法必须让代码嵌套关系与 retained tree 一致；
- Drawable geometry 和子 Container translation 都相对直接父 Container。Touch event 仍以 Scene 逻辑坐标
  投递；`Container::ToLocal()` / `ToScene()` 用于跨坐标空间，高层控件在命中测试前自动转成本地坐标；
- Scene root 隐含最终 viewport clip。Drawable 和显式 clip 的局部矩形可以越过 parent 或 root；Host 合成
  祖先 translation/clip 后只栅格化与 viewport 的最终交集，离屏预取不放宽 texture source 或 buffer 校验；
- `Sprite` 适合有独立身份的纹理对象，`SpriteBatch` 适合蛇身、方块、爆炸、粒子和 tile；`Shape` v1
  提供矩形，`Label` 使用 Host 字形缓存，`SurfaceNode` 显示可局部更新的 `StreamingTexture`；
- `Point`、`Rect` 和 `Size` 是 Renderer 与 Input 共用的逻辑坐标 value；显示尺寸只由 `RendererInfo`
  公开，`InputInfo` 不维护第二份尺寸来源；布局只使用 `width()` / `height()`，物理视口尺寸只用于把触摸
  距离等设备像素阈值换算到逻辑坐标；
- Board 的 `DisplayInfo` 以原生像素声明四边 safe-area inset。Graphics Service 把它追加到 `GET_INFO`，
  Guest SDK 向外取整为逻辑 `RendererInfo::safe_area_insets()` / `safe_area()`；圆角和异形屏适配由布局消费
  这一通用几何值，不把板名、面板型号或经验 padding 写进 App；
- 逻辑坐标变换属于 Guest SDK：SDK 根据物理屏幕建立短边为 720 的逻辑画布，并在序列化 Scene 前把
  geometry、Container translation、atlas source rect 和语义字体统一 lower 为物理值。Graphics wire 与 Host
  `GuestScene` 只处理物理坐标，不复制 Guest 的 layout 兼容判断；Scene descriptor 必须匹配当前
  `RendererInfo` 的逻辑尺寸；
- `SceneUpdate` 聚合一次逻辑更新并以 `Present()` 原子发布；失败时 Host current scene 不变；
- 常用路径可用 `Renderer::CreateScene(background)` 自动采用当前逻辑尺寸，并用 `Scene::Update(lambda)` 包装
  单次事务；显式 descriptor 和 `BeginUpdate()` / `Present()` 继续用于 conformance 与需要手工控制事务的代码；
- Scene node 和 Container 使用固定槽位及 generation handle，显式 `Destroy()` 在提交成功后归还槽位；
  销毁 Container 会递归销毁完整子树；创建和销毁都可参加 SceneUpdate，失败时恢复旧 handle、失效本事务
  新建 handle，并通过 generation 防止回滚创建与后续槽位复用发生别名；
- 页面生命周期可按产品需要选择常驻或按需：常驻页面通过根 Container 的 visibility 切换；按需页面进入时
  创建子树、退出时只销毁页面根 Container。两种策略不得把同一页面的对象拆成无所有权关系的平行分组；
- Guest App 可使用完整 C++23 STL 管理 handle 和页面模型；例如 Demo 的绘制节点用 `std::vector` 随当前
  需求增长，并在同一 SceneUpdate 销毁多余节点。Host/ABI 的节点、Container、instance 和 wire 容量仍是
  显式 capability，不因 Guest 动态容器而取消；
- Guest Scene 事务以 touched slot 为工作集：setter 对无变化值直接返回，普通 patch 只扫描触碰过的
  node/container/instance；Host compositor 根据最近一次成功 wire 的 property mask 增量更新 normalized
  operation。keyframe、结构变化和 Container z-order 变化仍保留完整验证、展开与排序路径；
- Graphics wire 首次发布完整 Scene keyframe，普通更新发布属性 patch。Patch 携带
  `scene_generation + base_revision + revision`，Host 在固定容量 scratch scene 中完整验证后才原子交换；
  revision 不匹配时 SDK 下一次自动发布 keyframe；节点创建、销毁和 Container 级联销毁同样发布新 keyframe，
  并把存活节点按创建顺序压缩为连续 wire Node ID；
- 局部震动和控件移动只 patch Container translation；Sprite 动画只 patch source、destination、opacity 或 visibility；
  SceneUpdate 以 `BeginUpdate()` 时的状态计算净差量，属性在同一事务中改回原值时不进入 wire。Snake 的
  body ring 复用尾槽，普通移动只发送尾槽、独立头部/眼睛和少量颜色分段边界的真实变化，不随蛇长线性
  重发整个 SpriteBatch；
- Guest 不能访问 Host DisplayRoot。HostCompositor 把整个 Guest Scene 作为 `GuestSceneLayer`，再与
  `StatusBarLayer`、`PullDownPanelLayer` 和 `SystemDialogLayer` 合成，所以下拉状态面板不进入 Guest ABI；
- Host 正常链路固定为 `GuestScene -> AppSurfaceCompositor -> App Surface damage -> LVGL Host root ->
  DisplayPipeline`。LVGL 中的 Guest 只有一个 App Surface image，系统 UI 保持原生对象；板级
  `DisplayPipeline` 是 panel、transport、flush 和显示 shadow 的唯一 owner，其他 compositor 不直接提交面板；
- App Surface 可配置 1–3 个（`CONFIG_MICROPIXEL_APP_SURFACE_COUNT`）。多 surface 时 Guest task 的
  `Submit` 全程不取 LVGL 锁：合成到一个既未显示也未 pending 的 surface，再把 surface 序号和 content
  damage 放进 spinlock 保护的单槽 mailbox 并唤醒 LVGL task；LVGL task 在 `LV_EVENT_REFR_START`（或常驻
  publish timer）里在 LVGL 锁内 adopt 该帧：切换 image data 指针并按 damage invalidate。Guest 领先时新帧
  直接替换未被采用的 pending 帧，damage 取并集。`AppSurfaceCompositor` 为每个 surface 记录 carry damage，
  present 到任意 surface 时先重放 carry，使所有 surface 增量收敛到同一内容。单 surface 配置退回锁内合成
  并就地 adopt。锁序固定：LVGL 锁 → publish spinlock；Guest task 永不在持 spinlock 时取 LVGL 锁。
  Raw 纹理在加载时从 flash 映射暂存到 PSRAM，保证 DMA2D/PPA 源始终在 PSRAM；
- 调试日志分别记录 Scene damage/重放数/Layer cache、PPA/DMA2D/CPU fallback、LVGL 合并前后区域，以及
  QSPI panel submit 与 DMA2D shadow copy，用于真机核对每个局部区域只有一次最终 RAM copy 和一次面板提交；
  分段计时边界、当前真机基线和优化判断见
  [Graphics 性能诊断与基线](../development/graphics-performance.zh-CN.md)；
- Snake 真机验收使用 480×480 屏幕：首次 keyframe 可以全屏；稳定普通移动的单次 wire 应不超过 16 个
  changed instances，damage 应小于屏幕 10% 且 `capacity-merges=0`；atlas 切帧应为单节点 patch；震动进入
  时必须出现 `layer-cache=yes`、translation-only wire，并把旧/新 Layer bounds 合成一个小于全屏的区域；
  `panel submit` 与 `shadow=dma2d` 的累计次数和像素数必须相等。超出这些条件属于性能回归，不以画面正确
  代替验收；
- v1 不提供 `AnimationClip/Track`。动画时序由 Guest 驱动，未来只有在真机传输数据证明需要时，才以完整的
  playback、时钟、暂停和完成事件语义扩展，不预留半套 opcode；
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

每个 AOT section 的 `reserved0` 保存 CPU target bitmap：bit 0 为 RISC-V `ilp32f`，bit 1 为 ESP32-S3
Xtensa，`0` 只用于没有该元数据的旧 Bundle。当前每个 AOT section 必须恰好设置一个已知 bit，v1 reader
仍要求 App 恰好包含一个 AOT section；新安装在任何 BundleFS staging write 前校验 target，旧或不匹配的
AOT 会被拒绝。把 target mask 绑定到 section 而不是 header，为未来一个 Bundle 携带多个架构 payload
保留了格式空间；各 section mask 的 OR 就是 Bundle 支持的架构集合，但多 AOT 的选择、唯一性和容量策略
尚未启用。

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

当前集成应用为 Blocks、Snake、Tilt、Demo 和四个 Showcase Bundle（Tap Counter、Color Lab、Pixel Sketch、
Orbit Pad）。Demo 覆盖公开 Service，Snake 覆盖高频 Graphics、Input、Storage 和 Audio，Blocks 覆盖
StreamingTexture 与批量 damage，Tilt 覆盖传感器输入、双分辨率 atlas 和长关卡进度，Showcase 覆盖多 App Hall、
轻量 Scene 和独立 Bundle 生命周期。

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
- FPS/CPU 蒙层只在 Guest App 前台运行时显示，App Hall、系统菜单、Status Layer、启动页和挂起状态隐藏；
- 顶部下滑打开 Status Layer；从底部中央三分之一区域上滑会暂停并返回大厅，左右两侧同类输入仍交给
  Guest；App 启动或恢复后的底部提示条显示 3 秒并自动隐藏；
- 恢复同一 App 时复用 Session 和 retained Scene，并由 PPA 从 Hall 卡片硬件放大回最后一帧；
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
4. Texture retained 生命周期、Timer 积压、Run/Stop 和八个集成应用的真机回归；
5. 生产 package 签名与授权、网络配置，以及在线安装/升级/卸载的完整断电和错误恢复矩阵。

## 9. 架构禁止项

- 不引入深继承树、万能 Manager、Service Locator 或新的全局可变状态；
- 不让 `FirmwareApp`、ABI adapter 或 Platform 承担本属于领域 Service 的业务规则；
- 不在 C ABI 中暴露 C++ class、`std::expected`、vtable、STL 类型或 Host 指针；
- 不用裸 `new/delete`、无界容器或 detached task 承担实时资源与生命周期；
- 不为新板型分叉 Guest API；板级差异通过 Device contract、Service capability 和 Bundle
  requirements 表达；
- 不修改第三方源码以迎合本项目格式、命名或文档结构。
