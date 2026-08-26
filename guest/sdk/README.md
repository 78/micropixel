# Guest C++ SDK

状态：**0.9.2，v1 事件循环已收敛。** 唯一标准入口是 `Run(event_handler)`；Timer 统一从
`app.timers().After/Every()` 创建。`WaitEvent()` 只用于需要有限等待或协议级控制的高级代码。

## 工具链兼容性

SDK 0.9.2 生成的 Guest 必须使用公开的
[MicroPixel WAMR fork](https://github.com/78/wasm-micro-runtime) 固定 commit
`77eb0f2ceb331e96ceab9737cc37f0b4a492781b` 编译为 AOT format v6。该编译器当前仍自报
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
- Timer、Touch、Key、Resume、Stop 和未来 Event 全部进入同一个 handler；
- Timer 通过 `event.TimerFrom(timer)` 匹配，资源身份和 capture 生命周期在代码中可见；
- `Stop` 会先交给 handler，handler 返回后 `Run()` 返回，应用随后从 `main()` 返回；
- SDK/Runtime 失败在发生点 panic，普通应用不写 `try`、`if (!result)` 或 ABI status 样板。

应用自身的前置条件或 invariant 使用带条件和原因的 `micropixel::AssertThat()`，不要返回无法从
日志理解的数字：

```cpp
micropixel::AssertThat(display.width() >= 320U && display.height() >= 320U,
                     "app: requires at least a 320x320 logical display");
```

不能自然写成条件的直接失败仍可使用 `micropixel::Panic(reason)`。不使用 `micropixel::assert` 作为函数名，
是为了避免它被标准 C/C++ `assert` 宏展开。

需要在一个短函数中只等待某个结果时，可以使用高级接口 `WaitEvent()`；它不应成为长期 App 的第二套
主循环模板。可运行的 SDK 用法按能力拆在 `guest/apps/demo/pages/`，由同一个 Demo Bundle 导航和测试；
详细协议验收位于 `guest/tests/conformance/`。

## Application façade 与 Public 对象分类

`Application` 是可通过自动补全逐层发现能力的 façade/capability root，不是实现所有能力的
“上帝对象”。`app.clock()`、`app.random()`、`app.log()`、`app.timers()`、`app.renderer()`、`app.input()`、
`app.resources()`、`app.storage()`、`app.audio()` 和 `app.localization()` 都按值返回轻量 **Service View**；它们不包含对应 Host Service 的
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
| Service View | 轻量、可复制、没有独立资源身份 | `Log`、`Clock`、`Random`、`Timers`、`Renderer`、`Resources`、`KVStore`、`Audio`、`Localization` |
| Resource | 有 Host 身份和所有权，默认 move-only、析构释放 | `Timer`、`Texture`、`StreamingTexture`、`Frame`、`TextureUpdateBatch` |
| Value | 普通可复制数据快照，不拥有 Host 资源 | `TimePoint`、`Duration`、`TimerEvent` |
| Module | 编译、链接或部署单元，不作为 `app.xxx()` 的返回对象 | Renderer SDK、Host Audio backend |

`Application` 只公开生命周期、事件编排和稳定的顶层能力入口。具体动作必须留在对应 Service 或
Resource 上：

```cpp
auto texture = app.renderer().CreateStreamingTexture(
    micropixel::Size{300U, 150U}, micropixel::PixelFormat::kBgr888);
micropixel::AssertThat(texture.has_value(), "texture allocation failed");

auto frame = app.renderer().BeginFrame();
frame.FillRect(rect, color);  // opacity 可省略，默认 255
frame.DrawTexture(micropixel::Point{47, 76}, texture.value());
frame.DrawTexture(micropixel::Point{x, y}, sprite, 160U);
micropixel::AssertThat(frame.Present().has_value(), "frame present failed");
micropixel::InputInfo input = app.input().info();
bool pressure_available = input.supports_pressure();
```

`app.audio()` 当前提供 Audio 1.0 的有界短音合成器。Guest 只提交 `Tone` 值；Host 在固定
8-voice pool 中混音，不接收 Guest PCM 指针，也不暴露 codec/I2S 细节。它适合 UI 音、游戏
音效及由 Timer 编排的短旋律。长 BGM、压缩音频和 streaming 将作为独立数据面演进：

```cpp
micropixel::Audio audio = app.audio();
audio.Play(micropixel::Tone{
    .waveform = micropixel::Waveform::kTriangle,
    .frequency_hz = 660U,
    .duration = 120_ms,
});
```

`Run(handler)` 是 Application 唯一的事件编排入口；Timer 操作归 `timers()`。新增 Camera、Storage
等能力时可以增加同级 Service View accessor 或 Event，但不得把 `DrawRect()`、`PlayPcm()`、
`TouchPosition()` 等叶子操作堆到 `Application`。

## 时间与 Event 来源类型安全

`Clock` 是随应用生命周期前进、未来在 Suspend 期间冻结的单调时钟；`TimePoint` 是该时间轴上的
值，`Duration` 是两个时间点之间的间隔：

```cpp
micropixel::TimePoint started = app.clock().Now();
micropixel::Duration elapsed = app.clock().Now() - started;
```

Public API 不允许 `Duration{1000}` 这种隐藏单位的构造。必须写成 `1000_us`、`1_ms` 或
`Duration::Milliseconds(1)`。非零 `TimePoint` 只能由 Runtime 通过 `Clock::Now()` 或 typed event
产生，应用不能用整数伪造另一个时间域的时间点。

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
`missed_count()` 返回未单独投递的 tick 数。`Timer::Cancel()` 只停止后续触发；`Timer::Reset()`
执行 best-effort cancel + release 并令对象失效，析构和 move assignment 也走 `Reset()`，不会 Panic。

`TouchEvent::x()/y()` 为 `int32_t`。只有 `InputInfo::supports_pressure()` 为 true 时，
`TouchEvent::has_pressure()` 才为 true，且 `pressure_per_mille()` 的范围为 0..1000；GT911 返回不支持和 0。
`InputInfo::supports_key_events()` 表示 Host 可以投递固定语义按键；它不承诺存在物理键盘。handler 可用
`event.key()` 取得 `KeyEvent`，读取方向、Confirm、Back、Menu、A/B/X/Y 以及 Down、Up、Repeat、Cancel。
只有 Repeat 的 `repeat_count()` 非零。
日志完整支持 `Debug()`、`Info()`、`Warning()`、`Error()` 四个等级。

## Service 演进与 ABI 隔离

Public C++ 方法不与 Wasm import 一一对应。新增 `Frame::DrawText()`、`Audio::Pause()` 或
`Input` capability 时，优先增加版本化 service method、payload field、event 或 command opcode，
不能机械增加同名 ABI 函数。

```text
Typed C++ Service View
    → 小数据：通用 Service Control Plane
    → 高频/大块数据：service_submit 的独立 channel
```

每个 Service 独立维护 major/minor 和 capability set，Host 在进入 `main()` 前校验 required
capability。旧 Guest 必须能在兼容的新 Host 上继续运行；新 Guest 对旧 Host 的可选能力应 fallback，
required 能力缺失则在启动前给出明确诊断。service/method ID、wire schema、resource handle 和
command protocol 全部由 SDK/Runtime 隐藏，AI 不直接填写。首版资源加载只有同步
`Resources::LoadTexture()`；未来如需异步加载，会增加独立的任务/请求对象，不改变现有同步方法的语义，
也不会把完成事件塞回通用 `Event`。

## Renderer、Frame 与 Texture

公开图形模型固定为五个对象：

- `Renderer`：可复制的设备入口，只负责查询信息、开始帧和创建 streaming texture；
- `Frame`：一次逻辑显示更新的命令记录器，必须显式 `Present()`；析构不会上屏；
- `Texture`：同步加载的只读、move-only Host 资源；
- `StreamingTexture`：可写脏矩形的 move-only Host 资源；
- `TextureUpdateBatch`：把多次 streaming texture 更新合并成一次 compositor 唤醒。

`RendererInfo::width()` / `height()` 是 Guest 逻辑坐标空间的唯一尺寸来源；SDK 不固定设备分辨率。
当前 Metalio Claw4 Host 返回 720×720，Input 必须报告同一个逻辑坐标空间，物理屏幕映射由 Host 负责。
应用不得把 720 写成跨设备常量。

应用不接触 `Layer`、`Surface`、transport batch 或 retained-compositor capability。需要局部平移时使用
普通渲染状态；SDK 会在支持的 Host 上自动使用 retained translation 快速路径，否则执行裁剪和平移降级：

```cpp
auto frame = app.renderer().BeginFrame();
frame.Clear(micropixel::Color::Black());
frame.Save();
frame.SetClipRect(board_bounds);
frame.Translate(micropixel::Point{shake_x, shake_y});
frame.DrawTexture(micropixel::Point{board_x, board_y}, board_texture);
frame.Restore();
micropixel::AssertThat(frame.Present().has_value(), "frame present failed");
```

v1 的 `Save`/`Restore` 最多嵌套 8 层；子状态继承父状态，`Translate()` 采用累加语义，子 clip 不能扩大
父 clip。每层的 clip 和 translation 必须在该层第一条绘制命令前设置。小帧直接一次 `service_submit`；超过 4096 bytes 时，SDK 才自动使用
Host frame begin/commit 做多批原子提交。应用不能手动提交 transport batch。未 `Present()` 的跨批帧
会在析构时 cancel，不会留下半帧状态。

`FillRect(rect, color, opacity)` 和 `DrawTexture(..., opacity)` 统一处理不透明与半透明
绘制。`opacity` 默认为 `255`；图片 opacity 与图片自身逐像素 alpha 相乘。不透明 texture 保留 Host copy
快速路径，不需要另一组 `Blend*` API。

Texture 有两组互补的绘制形式，宽高为 0 始终是非法空矩形，不承担“自动尺寸”语义：

```cpp
frame.DrawTexture(micropixel::Point{x, y}, texture);          // 原始尺寸
frame.DrawTexture(micropixel::Point{x, y}, texture, source);  // source 原始尺寸
frame.DrawTexture(destination, texture);                      // 整张纹理缩放到 destination
frame.DrawTexture(destination, texture, source);              // 裁剪并缩放
```

`Frame::draw_operation_count()` 与 `RendererInfo::max_draw_operations()` 只统计应用绘制操作；SDK 自动生成的
state、transport batch 和 retained acceleration 记录不计入预算，因此 Host 优化变化不会破坏应用的槽位计算。
`Present()` 返回 `Result<void>`；参数和状态编程错误仍会 trap，提交失败、容量耗尽等运行时错误可由应用处理。

文字使用语义字体角色而不是固定物理字号，坐标仍是 `RendererInfo` 给出的逻辑像素：

```cpp
const micropixel::Locale locale = app.localization().CurrentLocale(); // BCP 47，例如 en
frame.DrawText({24, 24}, "Hello", micropixel::Color::White(),
               micropixel::SystemFont::kLarge);
```

`SystemFont::{kSmall,kMedium,kLarge,kTitle}` 的实际字体和像素大小由 Host 决定。这样 Host 后续可在不改变
应用或 wire schema 的情况下选择不同语言字体；SDK 0.9.2 暂不提供翻译目录或语言包 API。

需要维护棋盘、画布或其他动态像素时，创建 `StreamingTexture`。格式名直接描述 Guest 内存字节顺序：
`kBgr888` 为 B/G/R，`kBgra8888` 为 B/G/R/A。

```cpp
auto board_result = app.renderer().CreateStreamingTexture(
    micropixel::Size{300U, 150U}, micropixel::PixelFormat::kBgr888);
micropixel::AssertThat(board_result.has_value(), "board texture allocation failed");
auto board = static_cast<micropixel::StreamingTexture&&>(board_result.value());

alignas(4) uint8_t cell[30U * 30U * 3U]{};
auto batch = app.renderer().BeginTextureUpdateBatch();
micropixel::AssertThat(
    board.Update(micropixel::Rect{60, 30, 30, 30}, cell, sizeof(cell), 30U * 3U).has_value(),
    "texture update failed");
micropixel::AssertThat(
    board.Update(micropixel::Rect{60, 60, 30, 30}, cell, sizeof(cell), 30U * 3U).has_value(),
    "texture update failed");
micropixel::AssertThat(batch.Finish().has_value(), "texture batch failed");

auto frame = app.renderer().BeginFrame();
frame.DrawTexture(micropixel::Point{47, 76}, board);
micropixel::AssertThat(frame.Present().has_value(), "frame present failed");
```

`Update()` 同时接收可读 `byte_length` 和每行 `pitch`；SDK 校验输入范围，并把大矩形自动切成不超过
4096 bytes 的有界 Resource call。Host 再校验 texture 类型、格式、bounds、pitch 和精确 payload 长度。
每个 streaming texture 计入 Guest 的 PSRAM 配额。

`sdk/ui/button.hpp` 提供无堆分配的 `ui::Button`。它捕获按下时的 touch id，手指移出时取消视觉
按下态，回到按钮内会恢复，只有在按钮内松开才返回 `clicked`。动作仍由 App 处理，渲染可选
`DrawTextButton()` 或 `DrawTextureButton()`。文本按钮默认叠加同一矩形上的半透明黑色蒙版；贴图按钮
按下时降低原图的 command opacity，并继续保留图片自身的逐像素 alpha，因此透明边缘和圆角不会被
矩形反馈层覆盖：

```cpp
micropixel::ui::Button play_button{{250, 316, 220, 72}};

if (const auto update = play_button.OnTouch(touch); update.clicked) {
    StartGame();
}

micropixel::ui::DrawTextButton(commands, play_button, "START GAME");
```

无需动态分配的短文本拼接统一使用 `FixedString<Capacity>`；Demo、Snake 和 conformance 日志共用
这一实现，不在各 App 内复制字符串类。

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
`raw_rgb888`、`raw_argb8888` 或 `png_to_raw_rgb888`，但这些 raw 格式不能被指定为 launch 封面。

多帧动画应优先打包为 sprite sheet/texture atlas。Guest 只同步加载一次 `Texture`，再通过
`Frame::DrawTexture(position, texture, source_rect)` 选择帧；切帧只更新 command 中的 source rect，
不触发资源查找、图片解码或 Texture 分配。

日常构建只把项目目录交给统一 CLI。它从 `app.json` 读取 sources、localization 和 asset manifest，生成
绑定头、编译 Guest，并把稳定 AppId、AOT 和资源 TOC 写入补齐到 64 KiB extent 的 Bundle v1：

```sh
python3 tools/micropixel build path/to/app
python3 tools/micropixel package path/to/app
python3 tools/micropixel install path/to/app
```

在包含 `app.json` 的目录中，安装后的 CLI 可直接运行 `micropixel build/package/install`，不需要 App
专用脚本。默认输出在项目的 `build/`；仓库集成 App 输出在 `build/apps/<name>/`。构建固定 Restricted
C++23、警告即错误、共享 Wasm memory 和 AOT 回跳中断点。`build` 默认 development（`-O1 -g`），
`package/install` 默认 release（Clang `-O2`、WAMR AOT opt level 3）；只有明确以体积优先时才使用：

```sh
python3 tools/micropixel package path/to/app --profile size
```

`tools/build_guest_app_p4.sh` 与 `tools/build_app_bundle.py` 仍是 conformance、底层调试和打包器测试使用的
内部构件，不是普通 App 的公开工作流。

USB 调试统一使用 `bash tools/p4.sh flash-apps` 写入 Blocks、Snake 和 Demo。自定义 Bundle 不再绕过
安装事务直接覆写分区，应通过 Remote Control 安装。

所有 import 必须在 `guest/abi/allowed_imports.txt` 中声明；未授权 import 和拼写错误在链接时
失败。AI 不应自行拼接工具链命令。

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

尚未定义的是 Network、Camera、长音频/压缩音频的具体 method/channel/resource 组合，以及是否在
Public 类型稳定后引入 Typed IDL/binding generator；它们不改变 v1 transport。

格式、命名、所有权和 Guest 限制见
[C/C++ 代码风格](../../docs/development/code-style.zh-CN.md)。底层协议见
[Runtime Host ABI](../abi/README.md)。
