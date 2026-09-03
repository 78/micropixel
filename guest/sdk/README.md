# Guest C++ SDK

状态：**0.12.1，v1 事件循环已收敛。** 0.12.1 修复自适应纹理缩放时 atlas 子矩形边界独立取整导致的
1 像素越界。唯一标准入口是 `Run(event_handler)`；Timer 统一从
`app.timers().After/Every()` 创建。`WaitEvent/WaitEventFor/PollEvent` 只用于短期等待或协议级控制。

## 工具链兼容性

SDK 0.12.1 生成的 Guest 必须使用公开的
[MicroPixel WAMR fork](https://github.com/78/wasm-micro-runtime) 固定 commit
`4dbe3b6efe776fde06468e47f342c1d351879cf0` 编译为 AOT format v6。该编译器当前仍自报
`wamrc 2.4.3`，但上游 WAMR 2.4.3、2.4.4 和 2.4.5 Release 都生成不兼容的 AOT v5；因此
`wamrc --version` 不是兼容性判断依据。安装步骤和产物头检查见 MicroPixel Developer 的
[开发环境](https://micropixel.ai/docs/environment/)页面。

MicroPixel 是项目正式名称。Public C++ API 使用 `micropixel` namespace 和 `sdk/micropixel.hpp`，
C ABI 统一使用 `micropixel_` 前缀。ABI 与 Renderer、Input 等后续能力仍须按各自里程碑用真实任务
验证后再冻结。

## AI-first 默认写法

普通 Guest 只包含 `sdk/micropixel.hpp`，实现标准 `int main()`：

```cpp
#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_s;

int main() {
    micropixel::Application app;
    micropixel::Timer timer = app.timers().Every(1_s);
    app.Run([&](const micropixel::Event& event) {
        if (const micropixel::TimerEvent* tick = event.TimerFrom(timer)) {
            (void)tick->delta();
            app.log().Info("tick");
        }
    });
    return 0;
}
```

这条默认路径使用统一事件语言表达意图：

- `app.timers().Every(period)` 创建周期 Timer；一次性 Timer 使用 `app.timers().After(delay)`；
- `app.Run(handler)` 是 Guest 自己的串行事件循环，不是 Host 回调，也不创建 Guest 线程；
- handler 可以返回 `EventResult::kContinue/kExit` 主动结束应用；返回 `void` 等价于始终 continue；
- Timer、Touch、Key、Resume、Stop 和未来 Event 全部进入同一个 handler；
- Timer 通过 `event.TimerFrom(timer)` 匹配，资源身份和 capture 生命周期在代码中可见；
- `Stop` 会先交给 handler，handler 返回后 `Run()` 返回，应用随后从 `main()` 返回；
- SDK/Runtime 失败在发生点 panic，普通应用不写 `try`、`if (!result)` 或 ABI status 样板。

应用自身的前置条件或 invariant 使用带条件和原因的 `micropixel::Assert()`，不要返回无法从
日志理解的数字：

```cpp
micropixel::Assert(display.width() >= 320U && display.height() >= 320U,
                     "app: requires at least a 320x320 logical display");
```

不能自然写成条件的直接失败仍可使用 `micropixel::Panic(reason)`。不使用 `micropixel::assert` 作为函数名，
是为了避免它被标准 C/C++ `assert` 宏展开。

需要由 Guest 主动结束并返回 App Hall 时，handler 返回明确结果；原有 `void` handler 保持适合长期 App：

```cpp
app.Run([&](const micropixel::Event& event) {
    return ShouldExit(event) ? micropixel::EventResult::kExit
                             : micropixel::EventResult::kContinue;
});
```

需要在短函数中读取事件时，可以使用高级接口：`WaitEvent()` 无限等待，
`WaitEventFor(event, timeout)` 有限等待，`PollEvent(event)` 不阻塞。后两者只在没有事件时返回 false；
Runtime/ABI 错误仍在调用点 panic。它们不应成为长期 App 的第二套主循环模板。可运行的 SDK 用法按能力
拆在 `guest/apps/demo/pages/`，由同一个 Demo Bundle 导航和测试；详细协议验收位于
`guest/tests/conformance/`。

## Application façade 与 Public 对象分类

`Application` 是可通过自动补全逐层发现能力的 façade/capability root，不是实现所有能力的
“上帝对象”。`app.clock()`、`app.random()`、`app.log()`、`app.timers()`、`app.renderer()`、`app.input()`、
`app.resources()`、`app.storage()`、`app.audio()`、`app.localization()`、`app.devices()`、
`app.sensors()`、`app.gpio()`、`app.haptics()` 和 `app.power_info()` 都按值返回轻量 **Service View**；它们不包含对应 Host Service 的
实现和资源状态。同类 Service View 的多个副本访问同一个 Guest Service：

```cpp
micropixel::Clock clock = app.clock();
micropixel::Clock same_clock = clock;

micropixel::TimePoint started = clock.Now();
micropixel::TimePoint current = same_clock.Now();
```

这里没有创建两条独立时间线。真正的当前时间快照是 `TimePoint`。Public 类型固定按以下语义分类：

| 分类 | C++ 语义 | 示例 |
| --- | --- | --- |
| Service View | 轻量、可复制、没有独立资源身份 | `Log`、`Clock`、`Devices`、`Sensors`、`Gpio`、`Haptics`、`PowerInfo` 等 |
| Resource | 有 Host 身份和所有权，默认 move-only、析构释放 | `Timer`、`Texture`、`Playback`、`Sensor<T>`、`GpioInput/Output/Pwm`、`Haptic` 等 |
| Value | 普通可复制数据快照，不拥有 Host 资源 | `TimePoint`、`DeviceInfo`、`SensorInfo`、`PowerState` 和 typed event 等 |
| Module | 编译、链接或部署单元，不作为 `app.xxx()` 的返回对象 | Renderer SDK、Host Audio backend |

`Application` 只公开生命周期、事件编排和稳定的顶层能力入口。具体动作必须留在对应 Service 或
Resource 上：

```cpp
auto texture = app.renderer().CreateStreamingTexture(
    micropixel::Size{300U, 150U}, micropixel::PixelFormat::kBgr888);
micropixel::Assert(texture.has_value(), "texture allocation failed");

auto scene = app.renderer().CreateScene();
auto game = scene.CreateContainer({.clip = {0, 0, 720, 720}});
auto board = game.CreateSurfaceNode(texture.value(), rect, source);
micropixel::Assert(scene.Update([&](auto& update) { board.SetOpacity(update, 192U); }).has_value(),
                   "scene update failed");
micropixel::InputInfo input = app.input().info();
bool pressure_available = input.supports_pressure();
```

`app.audio()` 提供 Audio 1.1。短 UI 音和程序化音效继续提交 `Tone` 值，由 Host 的固定 8-voice
pool 混音；BGM、对白和较长音效使用 Bundle 中的 `ogg_opus` asset。`AudioClip` 表示可重复播放的
压缩来源，`Playback` 表示一次可暂停、恢复、调音量和停止的播放，两者都是 move-only RAII 资源。
Host 负责 Ogg demux、Opus 解码、PCM ring buffer 和设备主音量，Guest 不接触 PCM 指针、codec 或 I2S。

```cpp
micropixel::Audio audio = app.audio();
audio.Play(micropixel::Tone{
    .waveform = micropixel::Waveform::kTriangle,
    .frequency_hz = 660U,
    .duration = 120_ms,
});

auto clip = audio.Load(game_assets::music_level_one);
micropixel::Assert(clip.has_value(), "load BGM failed");
auto playing = audio.Play(*clip, {.volume_per_mille = 260U, .loop = true});
micropixel::Assert(playing.has_value(), "play BGM failed");

app.Run([&](const micropixel::Event& event) {
    if (const auto* finished = event.PlaybackFrom(*playing)) {
        micropixel::Assert(finished->succeeded(), "BGM decode failed");
    }
});
```

当前 Host 上限为 16 个 clip handle、2 条同时 compressed playback；实际值应从 `AudioInfo` 查询。
播放开始后 Host 会 pin clip，所以关卡切换时可以先 `clip.Reset()` 释放 Guest 所有权，仍在播放的实例
不会失效；`Playback::Stop()`/析构或自然结束会撤销最后的 pin。跨多个关卡持续使用的 BGM 保留在上层
`AudioClip`/`Playback` 中，关卡专属素材则随关卡对象析构。`Audio::Play(AssetId, options)` 是只播放一次
时的便利写法。网络 URL、下载进度和缓存生命周期不属于 Audio 1.1，后续由 Resource/Network 加载层
提供相同的 source/playback 模型。

`Run(handler)` 是 Application 唯一的事件编排入口；Timer 操作归 `timers()`。新增 Camera、Storage
等能力时可以增加同级 Service View accessor 或 Event，但不得把 `DrawRect()`、`PlayPcm()`、
`TouchPosition()` 等叶子操作堆到 `Application`。

## 设备发现、传感器与 GPIO

应用不知道最终运行在哪块板上时，先枚举设备，再把不透明 `DeviceId` 交给对应能力 Service。枚举位置
不是身份，也不需要厂家预先给 GPIO 绑定用途：

```cpp
auto listed = app.devices().List();
micropixel::Assert(listed.has_value(), "device discovery failed");

for (micropixel::DeviceId id : *listed) {
    auto info = app.devices().GetInfo(id);
    if (!info) {
        continue;  // 热插拔设备可能已离开
    }
    if (info->kind == micropixel::DeviceKind::kGpioLine) {
        auto output = app.gpio().OpenOutput(id);
        if (output) {
            micropixel::Assert(output->Write(true).has_value(), "GPIO write failed");
        }
    }
}
```

`DeviceInfo::parent` 表达组合设备关系。例如未来两个无线手柄各有自己的 gamepad `DeviceId`，手柄里的
陀螺仪和加速度计可以作为独立 Sensor device，并把 parent 指向所属手柄；应用因此不会混淆两个手柄的
按键和传感器。设备目录只回答“有什么”，具体读取、配置与生命周期由 Sensors、GPIO、Haptics
等 Service 负责。

Sensor 按 reading 类型打开。当前提供 `Acceleration`、`AngularVelocity` 和 `MagneticField`，对应别名为
`Accelerometer`、`Gyroscope` 和 `Magnetometer`；后续温度、光照和压力会增加各自的 value type 与
`SensorTraits`，不会向现有 reading 塞入无关字段：

```cpp
using micropixel::literals::operator""_ms;

auto opened = app.sensors().Open<micropixel::Acceleration>(sensor_id);
micropixel::Assert(opened.has_value(), "accelerometer open failed");
micropixel::Accelerometer accelerometer = static_cast<micropixel::Accelerometer&&>(*opened);
auto configured = accelerometer.SetSampleInterval(10_ms);  // 100 Hz game sampling
micropixel::Assert(configured.has_value(), "sensor sampling rate unavailable");
auto sample = accelerometer.Read();
if (sample) {
    (void)sample->value.meters_per_second_squared.x;
}
```

第一个 Sensor `Open` 才让 Host 以游戏可用的 100 Hz 默认值向板级共享 I²C executor 注册周期采样；应用可通过
`SetSampleInterval()` 提升或降低采样率，允许范围由 `SensorInfo::minimum_interval` 和 `maximum_interval` 给出。
Host 将硬件 ODR 选择为不慢于请求值的档位，并按请求间隔刷新最新快照；返回的 `Duration` 是实际缓存
刷新间隔。低功耗边界是最后一个 handle 释放或 App Suspend，而不是已经打开的传感器。`Read()` 只复制
缓存，不等待 I2C 转换。刚打开、改频率或从 Suspend 恢复后的第一个采样周期内，
`Read()` 可以返回 `WouldBlock`。最后一个 Sensor handle `Reset()` 或析构后，周期采样被注销，芯片回到
suspend；Sensor 不创建独立任务。

`Sensor<T>`、`GpioInput/Output/Pwm` 和 `Haptic` 都是 move-only RAII resource。GPIO 打开即租用该引脚，
同一 Session 内其他 open 返回 `ResourceExhausted`；`Reset()`、析构或 Session teardown 释放并恢复安全
输入状态。PWM duty 与 Haptics strength 使用 0..1000，持续时间必须用 `Duration` 表达。

`GpioInputOptions::edge` 为 rising、falling 或 both 时，Guest 可用 `event.EdgeFrom(input)` 接收变化事件；
`edge = none` 时只支持主动 `Read()`。Host 只在至少存在一个 edge input 时运行 GPIO bridge task，不对引脚
轮询；output 和 PWM 不会启动该任务。

## 时间与 Event 来源类型安全

`Clock` 是随应用生命周期前进、未来在 Suspend 期间冻结的单调时钟；`TimePoint` 是该时间轴上的
值，`Duration` 是两个时间点之间的间隔：

```cpp
micropixel::TimePoint started = app.clock().Now();
micropixel::Duration elapsed = app.clock().Now() - started;
```

Public API 不允许 `Duration{1000}` 这种隐藏单位的构造。必须写成 `1000_us`、`1_ms` 或
`Duration::Milliseconds(1)`。非零 `TimePoint` 只能由 Runtime 通过 `Clock::Now()` 或 typed event
产生，应用不能用整数伪造另一个时间域的时间点。`Duration` 支持比较、加减、整数倍乘除；`TimePoint`
可以加减 `Duration`，溢出、下溢和除零会 trap：

```cpp
micropixel::Duration animation = 250_ms * 4U;
micropixel::TimePoint deadline = app.clock().Now() + animation;
```

Timer 是显式资源，handler 通过来源匹配获得 typed event：

```cpp
micropixel::Timer timer = app.timers().Every(50_ms);
app.Run([&](const micropixel::Event& event) {
    if (const micropixel::TimerEvent* tick = event.TimerFrom(timer)) {
        update(tick->delta());
    }
});
```

高级事件接口还提供 `TimerEvent::missed_count()`。周期 tick 合并时，`delta()` 累加真实经过时间，
`missed_count()` 返回未单独投递的 tick 数。`Timer::Cancel()` 是幂等终态操作：停止后续触发、释放
Host handle 并令对象失效。`Timer::Reset()` 是析构和 move assignment 使用的 best-effort release，
不会 Panic。需要再次调度时创建新的 Timer。

`TouchEvent::x()/y()` 为 `int32_t`。只有 `InputInfo::supports_pressure()` 为 true 时，
`TouchEvent::has_pressure()` 才为 true，且 `pressure_per_mille()` 的范围为 0..1000；GT911 返回不支持和 0。
`TouchEvent::position()` 直接返回与 Renderer 共用逻辑坐标空间的 `Point`；`Point`、`Rect` 和 `Size`
属于跨输入/图形共用的 geometry value，而不是某个 graphics backend 的类型。
`InputInfo::supports_key_events()` 表示 Host 可以投递固定语义按键；它不承诺存在物理键盘。handler 可用
`event.key()` 取得 `KeyEvent`，读取方向、逻辑动作 Confirm/Back/Menu，以及按物理位置命名的
South/East/West/North face button。Public API 不定义 A/B/X/Y，应用不得依赖不同手柄的标签布局。
阶段为 Down、Up、Repeat、Cancel，只有 Repeat 的 `repeat_count()` 非零。
日志完整支持 `Debug()`、`Info()`、`Warning()`、`Error()` 四个等级；消息 payload 最长为
`Log::kMaximumMessageBytes`，不包含结尾 NUL。

`Random::U32()` 返回完整 32-bit hardware random；需要 `[0, upper_bound)` 范围时使用
`Random::Below(upper_bound)`，它使用 rejection sampling 避免 `% upper_bound` 的 modulo bias。

`KVStore::GetBytesSize(key)` 先查询 byte value 的精确大小，再由应用选择固定数组或动态容器并调用
`GetBytes()`；这样 buffer-too-small 不需要退化成猜测固定上限。Package asset 与 app-private KV storage
保持为两个不同入口，不向 Guest 暴露文件系统路径。key 与 value 的硬上限分别由
`KVStore::kMaximumKeyBytes` 和 `KVStore::kMaximumValueBytes` 公开，UTF-8 key 按 bytes 计数且不包含 NUL。

## Service 演进与 ABI 隔离

Public C++ 方法不与 Wasm import 一一对应。新增 Scene 节点属性、`Audio::Pause()` 或
`Input` capability 时，优先增加版本化 service method、payload field、event 或 Scene record，
不能机械增加同名 ABI 函数。

```text
Typed C++ Service View
    → 小数据：通用 Service Control Plane
    → 高频/大块数据：service_submit 的独立 channel
```

每个 Service 独立维护 major/minor 和 capability set，Host 在进入 `main()` 前校验 required
capability。旧 Guest 必须能在兼容的新 Host 上继续运行；新 Guest 对旧 Host 的可选能力应 fallback，
required 能力缺失则在启动前给出明确诊断。service/method ID、wire schema、resource handle 和
Scene protocol 全部由 SDK/Runtime 隐藏，AI 不直接填写。首版资源加载只有同步
`Resources::LoadTexture()`；未来如需异步加载，会增加独立的任务/请求对象，不改变现有同步方法的语义，
也不会把完成事件塞回通用 `Event`。

Package 资源已经由生成代码表示为 `AssetId`，直接传给 loader；不再额外包一层只含同一个 ID 的
`ResourceRef::Package(...)`：

```cpp
auto texture = app.resources().LoadTexture(my_assets::background);
```

`LoadTexture()` 把以 720 短边逻辑画布制作的普通资源自适应到当前物理屏幕。只有 App 明确随 Bundle
提供并按 `RendererInfo::physical_width()` / `physical_height()` 选择物理分辨率变体时，才使用
`Resources::LoadNativeTexture()` 保留素材的原生像素尺寸；未精确匹配的显示 profile 必须回退到
`LoadTexture()`，不能把某个物理 profile 当作通用逻辑资源。

## Renderer、Scene 与 Texture

公开图形模型固定为以下对象：

- `Renderer`：可复制的设备入口，负责查询信息、创建 Scene 和 streaming texture；
- `Scene`：Guest 图形内容的唯一根节点，保存背景和对象集合；
- `Container`：`Scene` 与 `ContainerNode` 共用的子对象创建接口，receiver 就是 parent；
- `ContainerNode`：可嵌套的非绘制父节点，统一提供子树所有权、clip、translation、opacity、visibility 和
  z-order；
- `Sprite` / `SpriteBatch` / `Shape` / `Label` / `SurfaceNode`：保留式视觉对象；
- `SceneUpdate`：一次原子属性事务，必须显式 `Present()`；析构只放弃未提交事务；
- `Texture`：同步加载的只读、move-only Host 资源；
- `StreamingTexture`：可写脏矩形的 move-only Host 资源；
- `TextureUpdateBatch`：把多次 streaming texture 更新合并成一次 compositor 唤醒。

`RendererInfo::width()` / `height()` 是 Guest 布局所用逻辑坐标空间的唯一尺寸来源；
`physical_width()` / `physical_height()` 仅用于把触摸距离等物理像素阈值换算到逻辑坐标，不应用于布局；
`InputInfo` 不重复暴露第二份宽高。App manifest 不声明屏幕 profile。`Application` 初始化时由 SDK 读取物理屏幕
尺寸并建立短边为 720、长边按实际宽高比推导的逻辑画布。App 必须通过 `RendererInfo` 判断当前宽高和方向，
对不支持的布局在 Guest 入口给出明确 `Assert`；Host 不负责 Guest 布局判断。Scene geometry 和 Touch event 使用同一个逻辑坐标空间；SDK 在发送 Scene
keyframe/patch 前统一把节点矩形、Container translation、atlas source rect 和语义字体 lower 为当前屏幕的
物理值，Host Scene 只接收并验证物理坐标，不重复实现 Guest viewport 或布局规则。Scene descriptor 的
逻辑宽高必须等于当前 `RendererInfo`。自适应 UI 应直接依据 `RendererInfo` 布局。常规页面优先使用
`CreateFlexContainer()` 和 `CreateGridContainer()`：控件创建顺序就是布局顺序，Label 会缓存文本测量结果，
容器据此自动计算尺寸和对齐，不要求 App 保存每个控件的手工坐标。高层容器使用动态 STL 存储，只为实际
创建的控件和 Grid cell 分配内存，不常驻一组未使用的 Label/Shape 槽。例如 failure/summary 页面只需描述结构：

```cpp
auto panel = root.CreateFlexContainer(
    {.bounds = content_bounds,
     .layout = {.direction = micropixel::ui::FlexDirection::kVertical,
                .gap_pixels = 12,
                .distribution = micropixel::ui::FlexDistribution::kCenter,
                .alignment = micropixel::ui::FlexAlignment::kCenter}});
panel.CreateLabel("CRITICAL FAILURE", title_style);
panel.CreateLabel(score, score_style);

auto& stats = panel.CreateGridContainer({.columns = 3});
stats.CreateLabel("FOOD", muted_style);
stats.CreateLabel("MAX COMBO", muted_style);
stats.CreateLabel("LEVEL", muted_style);
stats.CreateLabel(food, value_style);
stats.CreateLabel(combo, value_style);
stats.CreateLabel(level, value_style);

panel.CreateTextButton(restart_properties);
micropixel::Assert(
    scene.Update([&](auto& update) {
        micropixel::Assert(panel.Layout(update).has_value(), "panel layout failed");
    }).has_value(),
    "scene update failed");
```

`FlexDirection::kHorizontal` 让子节点沿 X 主轴排列，`kVertical` 则沿 Y 主轴排列；`distribution` 始终控制
主轴剩余空间，`alignment` 始终控制交叉轴，所以切换方向时 App 不需要重新解释居中规则。Flex 支持
横向/纵向、固定像素、grow、padding、gap、主轴分布和交叉轴对齐；Label、TextButton、ImageButton 和嵌套
容器都可以直接作为子项。纯色文字按钮优先使用 TextButton，只有确实需要图片背景时才使用 ImageButton。
Grid 按 row-major 顺序自动推导行数并
等分列宽；目前不支持 wrap、span 或百分比。

`RendererInfo::safe_area_insets()` 返回已经换算为逻辑像素的四边安全内缩，`safe_area()` 返回对应的轴对齐
安全矩形。值由 Board 根据面板 Active Area、盖板和遮挡几何声明；圆角屏上的标题、状态值和触摸控件应以它
作为边缘基线，再叠加 App 自己的视觉 padding。普通矩形屏返回零 inset，App 不按板名或物理分辨率猜圆角。

需要直接计算矩形或实现自定义控件时，仍可使用底层 `ui::ComputeFlexLayout()` 和
`ui::ComputeGridLayout()`。它们是纯 Guest 侧的固定容量整数计算：应用提供 track/item 和输出 `Rect` span，
不创建控件树、不动态分配，也不调用 Host。Grid 的底层计算由一组纵向 row track 和一组横向 column track
组成，最多 8×8 cell。布局通常在应用启动、页面进入或内容尺寸变化时执行；触摸
事件已经是 Scene 逻辑坐标；直接挂在 Scene 下的 headless 控件不需要转换，Container 子树内的控件应先用
`container.ToLocal()` 转为本地坐标。`Resources::LoadTexture()` 会把 SDK 算出的短边缩放比例随请求发送给
Host；Host 在后台解码 PNG 后通过 PPA 一次性生成物理尺寸纹理。SDK 使用和资源请求相同的比例 lower
atlas source rect；纹理 destination 的尺寸独立取整，以保证逻辑 source/destination 同尺寸时在物理空间
仍严格同尺寸。纹理使用
`Sprite` 或 `SurfaceNode` 的 source/destination 同尺寸时保持 1:1 物理绘制，只有尺寸不同时
才走缩放路径。

Container 是局部坐标空间，不只是生命周期分组。每个新对象的 `x/y` 都相对接收创建调用的直接父
Container；嵌套 Container 的 `translation` 也相对其父 Container。`ToScene()` / `ToLocal()` 只在输入路由、
拖放或跨子树放置对象时跨越坐标空间，不参与 Host 的逻辑到物理缩放。`ui::TextButton` 等高层控件会自动把
Scene touch 转为自己的本地坐标，App 不重复换算。

应用直接创建 Scene 对象，但不接触 App Surface、transport generation 或 revision。首次提交发送完整
Scene keyframe，之后 `Present()` 只发送变化的对象属性。局部震动只改变 Game Container translation：

```cpp
auto scene = app.renderer().CreateScene();
auto game = scene.CreateContainer({.clip = board_bounds});
auto board = game.CreateSprite(board_texture, board_bounds, board_source);

micropixel::Assert(scene.Update([&](auto& update) { game.SetTranslation(update, {shake_x, shake_y}); }).has_value(),
                   "scene update failed");
```

滚动地图、tile 层这类“内容很少变、位置每帧变”的子树应放进带 `cache_content = true` 的 Container：

```cpp
auto viewport = scene.CreateContainer({.clip = view_bounds});
auto terrain = viewport.CreateContainer({.clip = view_bounds, .cache_content = true});
auto actors = viewport.CreateContainer({.clip = view_bounds});
// 每帧只平移两个 Container；terrain 子树本身不变时 Host 只从缓存复制。
scene.Update([&](auto& update) {
    terrain.SetTranslation(update, {-camera_x, 0});
    actors.SetTranslation(update, {-camera_x, 0});
});
```

`cache_content` 是渲染提示：Host 可以把该子树按局部坐标栅格化到保留缓存，平移只复制缓存，子树内容
改变时才重绘对应局部区域。缓存按不透明层合成，子树没有覆盖的像素显示 Scene 背景色，所以绘制顺序在
它之下的对象不会透出；会移动的角色、粒子应放在同级的普通 Container 里而不是缓存子树中。给它一个显式
clip，clip 就是缓存范围。`ContainerNode::SetCacheContent()` 可以随时切换该提示，Host 忽略该提示时绘制结果
不变。当前 Host 接受并校验该提示但尚未启用局部坐标缓存层：实测 tile 卷屏每帧真正变化的像素远少于整个视口，
现有 damage 路径已经更快，见 `docs/development/graphics-performance.zh-CN.md` 第 8 节。Host 目前用它选择
Layer 快照容器：第一个直接挂在 Scene 根上、带 `cache_content` 的 Container 成为 Layer；当它整体平移而子树
内容不变时（Snake 的震动），Host 捕获一次快照并复制，而不是重放子树。没有这样的 Container 时，第一个
Container 保持这一角色。

Scene 同时最多存在一个，Container 和对象容量由 `RendererInfo` 给出。应用不能手动提交 wire record、
generation 或 revision。`SceneUpdate` 析构会放弃未提交的属性事务，不产生半更新。
属性 dirty mask 表示相对于 `BeginUpdate()` 的净差量，而不是 setter 调用历史；例如先隐藏整个 Batch、再把
仍然存活的 instance 恢复为可见，不会把这些最终未变化的 visibility 写入 patch。

节点、Container、SpriteBatch instance 和事务 undo 使用按实际工作集增长的动态存储，容量仍受 ABI 上限
约束，并继续使用 slot ID + generation handle。删除对象后的空 slot 会优先复用；动态容器会保留已达到的
高水位供后续页面重建使用，但不会在 Scene 初始化时为全部上限或整套 undo snapshot 预分配内存。事务只为
本次真正修改的对象保存 undo。`Scene` 和 `ContainerNode` 共同提供 `Container` 创建接口；
创建调用的 receiver 就是 parent，不再提供 `scene.CreateX(parent, ...)` 形式。这个点语法让代码结构直接对应
对象树，也给每种对象的 Create 参数保留独立扩展空间；组合控件遵循同一规则，例如
`page.CreateTextButton(properties)`。页面可以按需创建一个 `ContainerNode`，再由它创建
drawable 或内层 container。销毁 container 会递归销毁完整子树并归还
槽位。销毁属于 `SceneUpdate` 事务：提交失败或
未调用 `Present()` 时会完整回滚，旧 handle 仍然有效；提交成功后旧 handle 失效，即使槽位随后复用也不能
误操作新节点。创建也可以发生在 active `SceneUpdate` 中，便于 `std::vector` 等 STL 容器在渲染时按需增长；
事务回滚后新 handle 失效，generation 保证它不会误命中以后复用的槽位。创建或销毁是结构变化，SDK 自动
发送新 keyframe；普通属性更新仍使用 patch：

```cpp
auto page = scene.CreateContainer();
auto dialog = page.CreateContainer({.translation = {40, 60}, .z_order = 10});
auto title = dialog.CreateLabel({24, 20}, "SETTINGS", micropixel::Color::White(),
                                micropixel::SystemFont::kLarge);

micropixel::Assert(
    scene.Update([&](auto& update) {
        dialog.SetVisible(update, false);  // 只隐藏对话框子树，页面其他节点不受影响
    }).has_value(),
    "page update failed");
```

Container 的 visible、opacity、translation 和 clip 沿父链继承，因此页面、面板、菜单内对话框都使用同一
棵树表达。只隐藏最内层 dialog 不影响外层菜单和兄弟节点；销毁外层 page 则递归销毁 dialog 和所有 drawable。
`ComputeFlexLayout()` 仍只是计算矩形的 Guest 工具，不创建另一棵控件树。输入事件以 Scene 坐标投递；
modal 路由由 App 决定，高层控件负责对自己的 Container 做本地坐标转换。

Sprite 的 destination 和 source 分别描述显示矩形与 atlas 区域；宽高为 0 是非法空矩形。图片 opacity 与
逐像素 alpha 相乘，不透明 texture 保留 Host copy 快速路径。蛇身、方块和粒子应使用 SpriteBatch：

```cpp
auto snake = game.CreateSpriteBatch(snake_atlas, 128U);
auto update = scene.BeginUpdate();
snake.SetInstance(update, tail_slot, {
    .destination = new_head_rect,
    .source = head_frame,
    .visible = true,
});
micropixel::Assert(update.Present().has_value(), "snake patch failed");
```

`RendererInfo::max_scene_nodes()`、`max_batch_instances()`、`max_containers()`、`max_sprite_batches()` 和
`max_scene_bytes()` 是明确容量。`Present()` 返回 `Result<void>`；参数和状态编程错误仍会 trap，提交失败、
容量耗尽等运行时错误可由应用处理。

普通矩形使用 `CreateShape()`；需要圆角、描边或二者组合时使用 `CreateRoundedRect()`：

```cpp
auto panel = scene.CreateRoundedRect(
    {40, 80, 320, 180},
    {.fill = micropixel::Color::Rgb(24, 28, 36),
     .stroke = micropixel::Color::Rgb(90, 220, 255),
     .radius = 24,
     .stroke_width = 3,
     .opacity = 224});
```

`RoundedRectNode` 与 Shape 一样只保存 retained 属性，不分配自己的像素 Surface。Host 仅在首次显示或
damage 重绘时把它直接混合进共享 App Surface；非零 radius/stroke 在短边过小时会安全截断。RGB565
Surface 支持绘制时 opacity 混合，但不保留可供以后拆分的独立 Alpha 通道。

文字使用语义字体角色而不是固定物理字号，坐标仍是 `RendererInfo` 给出的逻辑像素：

```cpp
const micropixel::Locale locale = app.localization().CurrentLocale(); // BCP 47，例如 en
auto title = scene.CreateLabel({24, 24}, "Hello", micropixel::Color::White(),
                               micropixel::SystemFont::kLarge);
```

`SystemFont::{kSmall,kMedium,kLarge,kTitle}` 的实际字体和像素大小由 Host 决定。这样 Host 后续可在不改变
应用或 wire schema 的情况下选择不同语言字体；SDK 0.12.1 暂不提供翻译目录或语言包 API。

`sdk/symbols.hpp` 提供所有 SystemFont role 都保证存在的稳定 UTF-8 图标，包括上下左右、播放/暂停、
确认/关闭、音量、文件、连接和电池状态。Guest 只提交普通文本，不包含 LVGL header，也不持有 Host 字体
对象：

```cpp
auto previous = panel.CreateTextButton(
    {.bounds = {20, 20, 80, 64},
     .text = micropixel::symbols::kLeft,
     .style = {.font = micropixel::SystemFont::kLarge}});
```

Public symbol 集合与 Host `builtin-latin-v1` profile 在 Host 构建时交叉校验，避免 SDK 已公开而某个板型
生成的字体缺字。

需要维护棋盘、画布或其他动态像素时，创建 `StreamingTexture`。格式名直接描述 Guest 内存字节顺序：
`kBgr888` 为 B/G/R，`kBgra8888` 为 B/G/R/A，`kRgb565` 为 little-endian 16-bit RGB565 word。RGB565
适合不透明、高频局部更新，可减少 Guest payload、Host texture 和 App Surface 带宽；透明像素仍使用
BGRA8888，由 Host 直接混合到目标 Surface。

```cpp
auto board_result = app.renderer().CreateStreamingTexture(
    micropixel::Size{300U, 150U}, micropixel::PixelFormat::kRgb565);
micropixel::Assert(board_result.has_value(), "board texture allocation failed");
auto board = static_cast<micropixel::StreamingTexture&&>(board_result.value());

alignas(4) uint16_t cell[30U * 30U]{};
auto batch = app.renderer().BeginTextureUpdateBatch();
micropixel::Assert(
    board.Update(micropixel::Rect{60, 30, 30, 30}, cell, sizeof(cell), 30U * 2U).has_value(),
    "texture update failed");
micropixel::Assert(
    board.Update(micropixel::Rect{60, 60, 30, 30}, cell, sizeof(cell), 30U * 2U).has_value(),
    "texture update failed");
micropixel::Assert(batch.Finish().has_value(), "texture batch failed");

auto board_node = scene.CreateSurfaceNode(board, {47, 76, 300, 150}, {0, 0, 300, 150});
auto update = scene.BeginUpdate();
micropixel::Assert(update.Present().has_value(), "surface present failed");
```

`Update()` 同时接收可读 `byte_length` 和每行 `pitch`；SDK 校验输入范围，并把大矩形自动切成不超过
4096 bytes 的有界 Resource call。Host 再校验 texture 类型、格式、bounds、pitch 和精确 payload 长度。
每个 streaming texture 计入 Guest 的 PSRAM 配额。

`sdk/ui/button.hpp` 提供无堆分配的 `ui::Button`。它捕获按下时的 touch id，手指移出时取消视觉
按下态，回到按钮内会恢复，只有在按钮内松开才返回 `clicked`。构造函数的可选 `hit_padding` 会在四边
扩大不可见触控区域而不改变绘制边界，适合小屏上的图标按钮。相邻按钮的扩大区域不应重叠；需要紧凑
排列时由 App 的页面级 hit tester 先选出唯一目标。动作和视觉绑定均由 App 处理；按下状态通常修改
按钮 Shape 的 opacity 或 Sprite 的 tint/opacity：

```cpp
micropixel::ui::Button play_button{{250, 316, 220, 72}, 12};

if (const auto update = play_button.OnTouch(touch); update.clicked) {
    StartGame();
}

auto scene_update = scene.BeginUpdate();
feedback.SetOpacity(scene_update, play_button.pressed() ? 48U : 0U);
micropixel::Assert(scene_update.Present().has_value(), "button update failed");
```

普通文字按钮优先使用 `sdk/ui/text_button.hpp` 的 `ui::TextButton`。它保留 `ui::Button` 的触摸语义，
并组合圆角背景、圆角状态遮罩和 Label；创建或修改文字/字体时缓存真实字体 metrics，之后按完整行框同时
水平、垂直居中，不在逐帧热路径测量文字。一个 `TextButton` 使用三个 Scene node；这些节点随控件实际创建，
不为未使用控件预留 Scene 存储。
`TextButton` 自己拥有一个 Container，背景、反馈层和文字都是它的子节点。把按钮挂到页面 Container 后，
页面退出只需销毁页面根 container，即可递归归还按钮 container 和三个 drawable 槽位：

```cpp
auto start_button = game.CreateTextButton(
    {.bounds = {250, 316, 220, 72},
     .text = "START",
     .style = {.background = micropixel::Color::Green(),
               .font = micropixel::SystemFont::kLarge,
               .corner_radius = 18},
     .hit_padding = 12});

const auto changed = start_button.OnTouch(touch);
if (changed.clicked) {
    StartGame();
}
if (changed.visual_changed) {
    micropixel::Assert(scene.Update([&](auto& update) { start_button.Sync(update); }).has_value(),
                       "text button update failed");
}

micropixel::Assert(game.Destroy().has_value(), "page destroy failed");
```

`TextButton` 默认使用 `TextOverflow::kClip`：自己的 Container 会裁剪到按钮 bounds，初始文字或后续
`SetText()`、`SetStyle()` 的实际行框放不下时仍保持双轴居中并安全裁剪，每个按钮生命周期最多输出一次
warning，不会终止 Guest。warning 会包含操作、当前按钮 `x/y/w/h`、测量得到的文字 `w/h` 和具体文案，
可直接定位发生溢出的控件；`text_clipped()` 可用于 App 诊断自适应布局。需要把溢出继续视为严格布局错误时，
在 properties 中显式指定 `.overflow = TextOverflow::kReject`。空文字、非法 bounds 和超过 128 bytes 的
文字仍然是无效参数；reject 模式的初始溢出会 trap，后续修改则返回 `kInvalidArgument` 且不会提交一半属性。

所有组合 UI 对象提供无堆分配的 `ToString()` 诊断快照。`Button` 包含视觉/命中 bounds 和捕获状态，`Label`
包含 bounds、metrics、对齐和文案，`TextButton`/`ImageButton` 额外包含 overflow policy 与 clipped 状态，
`FlexContainer`/`GridContainer` 包含布局 bounds、轨道或分类后的子对象数量。App 在处理 `Result` 失败时可直接
把对应快照写入日志；SDK 自己触发的控件 panic 也使用同一份状态格式，不再只报告泛化的控件类型。

图片自带边框、纹理或圆角时使用 `sdk/ui/image_button.hpp` 的 `ui::ImageButton`。它组合 Sprite、Label 和
`ui::Button`，图片 source 与最终 bounds 可以不同，文字仍按最终 bounds 的真实字体 metrics 双轴居中。若要缩放
带圆角的整张按钮图，应保持 source 的宽高比；需要任意拉伸时应改用九宫格素材，而不是让圆角随目标矩形变形。
`ImageButton` 与 `TextButton` 使用同一套默认 `TextOverflow::kClip`、单次详细 warning、显式 `kReject` 和
`text_clipped()` 语义：

```cpp
auto start = game.CreateImageButton(
    start_texture,
    {.bounds = {192, 295, 336, 115},
     .source = {0, 0, 280, 96},
     .text = "START GAME",
     .style = {.font = micropixel::SystemFont::kLarge}});
```

纯图标或完全定制的按钮继续使用 headless `ui::Button`。

无需动态分配的短文本拼接统一使用 `FixedString<Capacity>`；`Append*()` 返回内容是否完整写入，
`truncated()` 会在任一拼接被截断后保持 true，直到 `Clear()`。`capacity()` 返回不含 NUL 的最大内容
长度。Demo、Snake 和 conformance 日志共用这一实现，不在各 App 内复制字符串类。

完整规则见 [Guest–Host ABI](../abi/README.md)。

Bundle 资源清单使用语义名称，不允许 App 手写 TOC 数字 ID。Bundle builder 对名称做唯一性、
格式、动画序列连续性和 32 位 ID 冲突校验。prepare 阶段通过同一次清单解析原子生成包含
`AssetId` 及可选 atlas 布局的 C++ 绑定和不可变 `resources.pack`；字符串和名称查找不会进入 Guest 运行时或
Resource Service ABI。例如清单中的 `button.start` 和
`food.normal.00..15` 会生成 `snake_assets::button_start` 与定长的
`snake_assets::food_normal[]`。资源重排不会改变由名称产生的内部 ID，名称拼写或帧数不一致则在
生成或编译阶段失败。资源清单及其引用的最终资源属于 App 源码并纳入版本控制；资源制作工具不参与
App 的 production 构建。资源 pack 携带版本、launch ID、逐项内容 hash 和整个目录的 SHA-256；
finalize 阶段只读取 AOT 和 pack，不会再次读取资源清单或重新分配 ID：

首版 `launch_asset` 是 Host 专用的 JPEG/PNG 封面，不经过 Guest ABI 或 Resource Service。默认应为不透明
封面使用 JPEG，以利用 ESP32-P4 硬件解码；需要透明背景或无损像素时使用 PNG。普通游戏资源仍可使用
`raw_rgb888`、`raw_argb8888`、`raw_rgb565`、`png_to_raw_rgb888` 或 `png_to_raw_rgb565`，但这些 raw
格式不能被指定为 launch 封面。`png_to_raw_rgb565` 只接受不透明 PNG；带 alpha 的资源必须保留
PNG/BGRA8888，避免静默丢失透明度。

多帧动画应优先打包为 sprite sheet/texture atlas。Guest 只同步加载一次 `Texture`，再通过
`SpriteNode::SetSource()` 或 `SpriteBatch::SetInstance()` 选择 atlas frame；切帧只提交 source rect 差量，
不触发资源查找、图片解码或 Texture 分配。v1 不提供 `AnimationClip/Track`，动画时间由 Guest 游戏循环
驱动；未来若加入 Host timeline，必须同时定义时钟、暂停/恢复、打断、资源 pin 和完成事件。

日常构建只把项目目录交给统一 CLI。它从 `app.json` 读取 sources、localization 和 asset manifest，生成
绑定头、编译 Guest，并把稳定 AppId、AOT 和资源 TOC 写入补齐到 64 KiB extent 的 Bundle v1：

```sh
python3 tools/micropixel build path/to/app
python3 tools/micropixel package path/to/app --aot-target riscv32-ilp32f
python3 tools/micropixel app install path/to/app
```

在包含 `app.json` 的目录中，安装后的 CLI 可直接运行 `micropixel build`、`micropixel package` 和
`micropixel app install`，不需要 App
专用脚本。默认输出在项目的 `build/`；仓库集成 App 输出在 `build/apps/<name>/`。构建固定 Restricted
C++23、警告即错误和 AOT 回跳中断点。`threading` 缺省为 `none`，此时不启用 atomics、Wasm shared
memory 或 wamrc multi-thread，linear memory 按需增长；只有显式声明 `shared-memory` 的 Bundle 才启用
这些编译特征。Host 会校验 Bundle 声明与 AOT target-info，避免错误标记绕过内存策略。`build` 默认
development（`-O1 -g`），
`package`/`app install` 默认 release（Clang `-Oz`、WAMR AOT opt level 3）。Release 保留软件越界检查和
内存诊断，只把 AOT 调用栈缩减为 instruction pointer + function index；development 保留完整调试信息和
完整调用栈。连接设备的 `app install`/`run`
根据设备芯片自动选择 AOT target；离线 `package` 必须显式传入 `--aot-target riscv32-ilp32f` 或
`--aot-target xtensa`。只有明确以体积优先时才使用：

```sh
python3 tools/micropixel package path/to/app --profile size --aot-target riscv32-ilp32f
```

需要构建、安装、启动并持续观察 Guest 日志时，可运行 `micropixel run`。它默认使用 development profile，
在完整打包成功后才停止当前 Guest；安装或启动失败时会尽力恢复此前运行的 App。`micropixel run --no-follow`
在启动后立即返回。对于已经安装的 App，`micropixel app start --follow` 会从当前 `app.json` 推导 App ID；
`Ctrl-C` 只断开日志跟随，不停止 App。

调试某个特定状态时，在 `--` 后传入本次 AppSession 的启动参数：

```sh
micropixel run path/to/game -- --level 100
# 已安装的 App 也可使用同一语法
micropixel app start com.example.game -- --level=100
```

Guest 保持标准无参 `int main()`，通过只读 SDK view 获取参数：

```cpp
#include <charconv>
#include <string_view>
#include <micropixel.hpp>

int main() {
    micropixel::Application app;
    uint32_t level = 1;
    if (const char* value = app.launch_arguments().FindValue("--level")) {
        const std::string_view text(value);
        (void)std::from_chars(text.data(), text.data() + text.size(), level);
    }
    // 使用 level 初始化游戏，然后进入 app.Run(...)
}
```

`FindValue()` 同时识别 `--level 100` 和 `--level=100`；`count()`/`Get(index)` 可用于自定义解析。
Host 最多接受 16 项、合计 512 bytes（含每项结尾 NUL）的 UTF-8 参数。参数只属于本次新建的
AppSession；从 Hall 暂停/恢复不会重新传参，普通 Hall 启动得到空列表。

`tools/build_guest_p4.sh` 与 `tools/build_app_bundle.py` 仍是 conformance 和打包器测试使用的
内部构件，不是普通 App 的公开工作流。

USB 调试统一使用 `bash tools/p4.sh flash-apps` 写入七个示例 App。自定义 Bundle 不再绕过
安装事务直接覆写分区，应通过 USB Local Control 或 Remote Control 安装。

所有 import 必须在 `guest/abi/allowed_imports.txt` 中声明；未授权 import 和拼写错误在链接时
失败。AI 不应自行拼接工具链命令。

## Guest STL profile

Guest 使用 wasi-sdk 33 的 no-exception libc++ headers 和静态库。`micropixel build` 始终开启
function/data sections 和 linker GC，因此没有实例化或引用的模板、函数和运行库对象不会进入最终
Wasm/AOT。SDK Demo 使用 `std::array`/`std::span` 管理原有页面表；Blocks 和 Snake 的生成 Catalog
使用 `std::array<std::string_view>` 与 `std::span`，但 SDK Demo 本身不接入 localization。

首版持续验证的 no-WASI 子集包括：

- `array`、`span`、`string_view`、`optional`、`variant` 和常用 `algorithm`；
- `new/delete`、`nothrow new`、aligned new/delete、`unique_ptr`；
- `string`、`vector`、`map`、`queue` 和默认底层 `deque`。

首次实际使用动态分配时，linker 才保留单线程 allocator。allocator 管理 linker `__heap_base` 之后的
Wasm linear memory，不再预留固定 32 KiB 数组。ESP32-P4 与 ESP32-S31 当前都把每个 Guest 的策略上限
设为 8 MiB，实例化时再根据最大连续 PSRAM 块下调实际上限；后续 `memory.grow` 只有在增长后仍高于 Host
安全水位时才会成功。该预算同时容纳静态数据、16 KiB auxiliary stack 和 C++ 动态分配，因此动态可用量会
低于实际 linear-memory 上限。Texture/offscreen surface 等 Host-owned PSRAM 资源不占 C++ heap，也不再
使用固定累计 Guest 配额；每次资源分配都按实时空闲量和最大连续块动态准入。

普通 `new` 的 OOM 按 SDK 不可恢复错误策略记录并 panic；`new (std::nothrow)` 返回 `nullptr`。业务所有权
仍使用容器或 RAII，不能用裸 `new/delete` 表达长期所有权。Guest AOT 保留 16 MiB 格式扩展上限，Host
通过 WAMR instantiation policy 施加当前最多 8 MiB、低内存时更小的实际上限；应用不能自行扩大 Host
policy。

当前 Host 为 Guest 触发的 PSRAM 分配保留 2 MiB 安全水位。若实例化前最大连续块已经不足以同时容纳
Guest 初始 linear memory 和该安全水位，应用会收到内存不足；已经启动的轻量 App 不会因为另一个大型 App
的理论上限而预占内存。

这不是 WASI/POSIX 环境：thread、mutex、filesystem、socket、locale/iostream 和依赖系统调用的标准库
能力不受支持，也不能新增 WASI import。exception、RTTI 和 reference-types 继续关闭；Public SDK 和
Guest–Host ABI 不暴露 STL 类型。`Result<T>` 继续作为 SDK 的稳定错误类型，应用内部可以自由使用上述
STL 子集。

## 错误策略：在错误发生点终止

Core API 不提供机械镜像的 `try_*` 方法。事件等待、Timer 创建/启动、`Cancel()` 和 `info()`
失败，表示程序错误、Runtime/ABI 故障或无法继续满足应用的基本资源要求。SDK 在原始调用点
输出 panic operation 与 ABI status，随后触发 Wasm trap：

```text
guest panic
timers.every.start
invalid_argument
```

Host 捕获 trap、Wasm 调用栈和这些结构化字段，清理 Guest 资源并把完整诊断交给调试器或 AI。
应用主动调用 `micropixel::Panic(reason)` 时，同样先输出 `guest panic` 和具体原因，再触发 trap。
`main()` 返回非零值仍可用于 conformance 测试区分失败分支；用户应用的不可恢复错误应优先使用
带原因的 panic。成功的短任务返回 `0`，长期 App 通常停留在 `app.Run()`。

`Result<T>` 与 `Error` 只由确实需要它们的 Service 头引入。只有“失败是正常业务结果且调用方
能采取不同动作”的接口才返回它们。当前 `KVStore` 的 key 不存在，以及 Texture 不存在、解码失败或
配额不足属于这类业务分支；未来还包括网络失败、权限被拒绝或异步操作取消。Core Timer/Renderer 等不可恢复
Runtime 错误仍在发生点 panic。

## Resume 与 Stop 事件

Host 从 App Hall 或状态层恢复同一个 AppSession 时，`Run()` handler 首先收到
`EventType::kResume`。暂停期间 App Clock、Timer、输入和音频都被冻结；恢复不会重新进入 `main()`。
Host 会先直接显示暂停前保留的 Guest 画面，所以画面恢复不依赖 Guest 及时提交新帧；需要重建动态内容的
App 可以在收到 `kResume` 后主动完整重画。

切换或关闭 AppSession 时，handler 收到一次 `EventType::kStop`；handler 返回后 `Run()` 自动返回。
Host 在 500ms 后仍未完成时才会强制终止，以保证单 App 约束和系统大厅始终可响应。
应用也可返回 `EventResult::kExit` 主动结束；Host 将成功返回的 `main()` 视为正常退出并返回 App Hall。

`CurrentLocale()` 在 AppSession 启动时确定并在本次运行期间保持不变。用户只能返回 App Hall 后修改
系统语言；下一次启动 Guest 时获得新的 effective locale，因此 v1 不提供 locale-change 事件。

`Result<T>` 是 freestanding Guest 对 `std::expected<T, Error>` 的兼容子集。应用使用
`has_value()`/`operator bool()`、`operator*`、`operator->`、`value()`、`error()` 和 `value_or()`；
失败值可由 `unexpected(Error{...})` 构造。Guest 关闭异常，因此错误状态读取 value 或成功状态
读取 error 会 trap，而不是抛出 `std::bad_expected_access`。完整标准库兼容不是目标，组合接口
只在实际 API 需要时增加。

## Watchdog 语义

1 秒 watchdog 是“连续 Guest 计算预算”，不是 `main()` 的总寿命：

- 阻塞在 `Run()` 内部的事件等待或显式 `WaitEvent()` 时暂停；
- 每次进入 Host ABI 时重新计时；
- AOT 在循环回跳处检查异步终止标志，因此没有 Host 调用的死循环也能可靠停止；
- timeout、trap 和非零退出都会由 Host 清理资源并报告 failure。

因此正常事件应用可以永久运行，单次 handler 或两次 Host 调用之间的纯计算不能无限占用 CPU。

## 设计边界

- `Application` 是可发现的 capability façade；高层只提供 `Run(handler)`，accessor 按值返回
  copyable Service View，高级层保留阻塞事件读取；
- Service View 没有独立资源身份；Service 创建的 Resource 才以 move-only RAII 表达 Host
  handle 所有权；
- Public API 不把 C ABI 的失败模型逐行泄漏给普通应用；
- `Timer` 是 move-only RAII proxy，析构自动释放 Host 资源；
- `Event` 和 `TimerEvent` 是 value type，不暴露 raw source、sequence、handle 或 wire buffer；
- `guest/runtime/sdk.cpp` 集中执行 C ABI lowering 和错误码映射；
- `guest/runtime/startup.cpp` 在 C++ 构造器和应用 `main()` 前检查核心 ABI；
- capability 不能只靠 C++ 构造权限保证安全，Host 仍须验证 handle、类型、generation 和所属
  Guest。

高级接口的 `TimerFrom()` 和 `touch()` 返回当前 `Event` 内 typed payload 的
受限 view；指针不得保存到该 `Event` 生命周期之外。48-byte wire event 只在 Runtime 中按
`service_id + event_id` 解码，不把 raw tag/payload 暴露给应用。

## Core 定稿边界

v1 已确定 `Application` façade、typed Service View、move-only Resource、Value/Event 和
`Run(handler)` 控制流。底层收敛为七个 Core imports、按 Service 版本协商、48-byte Event、
`service_call` 控制面和 `service_submit` 数据面。后续能力应沿用这些对象语义和错误策略，不能重新
把叶子操作或 ABI 状态码堆回 `Application`。

尚未定义的是 Network、Camera、网络资源加载/进度/缓存的具体 method/channel/resource 组合，以及是否在
Public 类型稳定后引入 Typed IDL/binding generator。Bundle Ogg Opus 已由 Audio 1.1 的
`AudioClip`/`Playback` 定义；未来网络加载只增加 source 的取得方式，不改变播放实例语义或 v1 transport。

格式、命名、所有权和 Guest 限制见
[C/C++ 代码风格](../../docs/development/code-style.zh-CN.md)。底层协议见
[Runtime Host ABI](../abi/README.md)。
