# Guest C++ SDK

状态：**0.7，阶段 A Core Public API 已定稿。**

MicroPixel 是项目正式名称。Public C++ API 使用 `micropixel` namespace 和 `sdk/micropixel.hpp`，
C ABI 统一使用 `micropixel_` 前缀。ABI 与 Graphics、Input 等后续能力仍须按各自里程碑用真实任务
验证后再冻结。

## AI-first 默认写法

普通 Guest 只包含 `sdk/micropixel.hpp`，实现标准 `int main()`：

```cpp
#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_s;

int main() {
    micropixel::Application app;
    app.Run(app.Every(1_s, [&] {
        app.log().Info("tick");
    }));
}
```

这条默认路径用任务语言表达意图：

- `app.Every(period, callback)` 创建并启动周期任务；一次性任务使用 `app.After(delay, callback)`；
- `app.Run(...)` 是 Guest 自己的串行事件循环，不是 Host 回调，也不创建 Guest 线程；
- 多个任务直接传给同一个 `Run()`，例如 `app.Run(app.Every(...), app.After(...))`；
- callback 默认可以不接收参数；需要实际间隔时可接收 `const TimerEvent &` 并读取 `delta()`；
- one-shot callback 触发前自动释放其 Host Timer；其他 move-only 资源仍由 RAII 管理；
- SDK/Runtime 失败在发生点 panic，普通应用不写 `try`、`if (!result)` 或 ABI status 样板。

应用自身的前置条件或 invariant 使用带条件和原因的 `micropixel::AssertThat()`，不要返回无法从
日志理解的数字：

```cpp
micropixel::AssertThat(display.width() == 720U,
                     "app: display width must be 720");
```

不能自然写成条件的直接失败仍可使用 `micropixel::Panic(reason)`。不使用 `micropixel::assert` 作为函数名，
是为了避免它被标准 C/C++ `assert` 宏展开。

需要完全自定义事件分发时，可以使用高级接口 `timers().Every()`、`WaitEvent()` 和
`event.TimerFrom(timer)`。可运行的 SDK 用法按能力拆在 `guest/apps/demo/pages/`，由同一个 Demo
Bundle 导航和测试；详细协议验收位于 `guest/tests/conformance/`。

## Application façade 与 Public 对象分类

`Application` 是可通过自动补全逐层发现能力的 façade/capability root，不是实现所有能力的
“上帝对象”。`app.clock()`、`app.random()`、`app.log()`、`app.timers()`、`app.graphics()`、`app.input()`、
`app.resources()`、`app.storage()` 和 `app.audio()` 都按值返回轻量 **Service View**；它们不包含对应 Host Service 的
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
| Service View | 轻量、可复制、没有独立资源身份 | `Log`、`Clock`、`Random`、`Timers`、`Graphics`、`KVStore`、`Audio` |
| Resource | 有 Host 身份和所有权，默认 move-only、析构释放 | `Timer`、`LoadRequest`、`Bitmap`、`OffscreenSurface` |
| Value | 普通可复制数据快照，不拥有 Host 资源 | `TimePoint`、`Duration`、`TimerEvent` |
| Task/Subscription | 保存 callback 和事件来源关系，默认 move-only | `TimerSchedule`、未来的 `TouchSubscription` |
| Module | 编译、链接或部署单元，不作为 `app.xxx()` 的返回对象 | Graphics SDK、Host Audio backend |

`Application` 只公开生命周期、事件编排和稳定的顶层能力入口。具体动作必须留在对应 Service 或
Resource 上：

```cpp
auto surface = app.resources().CreateOffscreenSurface(300U, 150U,
    micropixel::SurfacePixelFormat::kRgb888); // Host PSRAM 中的可写 Bitmap
commands.FillRect(rect, color);        // 叶子操作
commands.DrawBitmap(47, 76, surface.bitmap()); // surface 可直接参与普通合成
commands.BlendBitmap(x, y, sprite, 160U); // 单次绘制的整体透明度
app.input().OnTouch(callback);          // 属于 Input，不提升为 app.OnTouch()
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

`After/Every/Run` 保留为应用级高频任务语言；底层 Timer 操作仍归 `timers()`。新增 Camera、Storage
等能力时可以增加同级 Service View accessor，但不得把 `DrawRect()`、`PlayPcm()`、
`TouchPosition()` 等叶子操作堆到 `Application`。

## 时间与 callback 类型安全

`Clock` 是随应用生命周期前进、未来在 Suspend 期间冻结的单调时钟；`TimePoint` 是该时间轴上的
值，`Duration` 是两个时间点之间的间隔：

```cpp
micropixel::TimePoint started = app.clock().Now();
micropixel::Duration elapsed = app.clock().Now() - started;
```

Public API 不允许 `Duration{1000}` 这种隐藏单位的构造。必须写成 `1000_us`、`1_ms` 或
`Duration::Milliseconds(1)`。非零 `TimePoint` 只能由 Runtime 通过 `Clock::Now()` 或 typed event
产生，应用不能用整数伪造另一个时间域的时间点。

Timer callback 只接受两种形态，其他签名在编译期给出直接诊断：

```cpp
app.Every(50_ms, [&] { update(); });
app.Every(50_ms, [&](const micropixel::TimerEvent& tick) { update(tick.delta()); });
```

## Service 演进与 ABI 隔离

Public C++ 方法不与 Wasm import 一一对应。新增 `Graphics::DrawText()`、`Audio::Pause()` 或
`Input` capability 时，优先增加版本化 service method、payload field、event 或 command opcode，
不能机械增加同名 ABI 函数。

```text
Typed C++ Service View
    → 小数据：通用 Service Control Plane
    → 异步：Request Resource + typed Event
    → 高频/大块数据：service_submit 的独立 channel
```

每个 Service 独立维护 major/minor 和 capability set，Host 在进入 `main()` 前校验 required
capability。旧 Guest 必须能在兼容的新 Host 上继续运行；新 Guest 对旧 Host 的可选能力应 fallback，
required 能力缺失则在启动前给出明确诊断。service/method ID、wire schema、resource handle 和
command protocol 全部由 SDK/Runtime 隐藏，AI 不直接填写。

Graphics 的 Surface translation 是可选能力。应用先通过
`graphics.info().supports_surface_translation()` 查询，再用 `BeginSurface()` / `EndSurface()`
把一组绘制命令标记为可整体平移的图层；不支持时必须保留普通绘制路径。Surface 描述的是合成
边界，不承诺 Host 使用哪一种 GPU、DMA2D 或缓存实现。

`OffscreenSurface` 是另一种、与 Surface translation 正交的资源：前者是 Guest 可局部写入的
Host-owned Bitmap，后者只是 CommandBuffer 中一组对象的可平移合成边界。需要维护棋盘、画布或粒子层时，
通过 Resource 1.2 创建 offscreen surface；创建时只分配一次 Host PSRAM，之后上传脏矩形。一个逻辑帧
涉及多个区域时，用 `OffscreenUpdateFrame` 建立原子呈现边界：

```cpp
auto board = app.resources().CreateOffscreenSurface(
    300U, 150U, micropixel::SurfacePixelFormat::kRgb888);

// RGB888 使用 LVGL little-endian 原生布局：每个像素 B, G, R。
alignas(4) uint8_t cell[30U * 30U * 3U]{};
auto frame = app.resources().BeginOffscreenUpdateFrame();
board.Update(micropixel::Rect{60, 30, 30, 30}, cell, 30U * 3U);
board.Update(micropixel::Rect{60, 60, 30, 30}, cell, 30U * 3U);
frame.Commit();

commands.DrawBitmap(47, 76, board.bitmap());
```

`Update()` 的输入 stride 至少覆盖一行；SDK 将较大的矩形自动切成不超过 4096 bytes 的有界
Resource call。Host 校验 handle、格式、bounds 和精确 payload 长度，在显示互斥锁内逐行写入。Frame
期间 Host 按 backing Bitmap 合并 damage；`Commit()` 在同一个显示锁临界区完成全部 invalidate 并只唤醒
一次 compositor，防止逐矩形中间态上屏，也让相邻小区域合并后进入 PPA 门槛。静止画面不需要重复提交 CommandBuffer。
每个 surface 计入该 Guest 现有 Bitmap PSRAM 配额，`OffscreenSurface` 保持 move-only RAII 语义。

Graphics 直接提供 source-over 合成：`BlendRect(rect, color, opacity)` 绘制半透明纯色，
`BlendBitmap(x, y, bitmap, opacity)` 和 `BlendBitmapRegion(...)` 为一次贴图绘制设置统一
`opacity`。这里的 opacity 不是应用或 Surface 的全局状态；它只属于当前 command，并与图片自身的
逐像素 alpha 相乘。`0` 完全透明，`255` 完全不透明。普通不透明贴图继续使用 `DrawBitmap()`；Host
会为它保留 copy 快速路径。只有确实需要淡入、受伤闪烁、阴影或半透明贴图时才用 `BlendBitmap()`，
Host 可在 PPA 上一次完成合成，不需要先生成一张临时图片再画第二遍。

`sdk/ui/button.hpp` 提供无堆分配的 `ui::Button`。它捕获按下时的 touch id，手指移出时取消视觉
按下态，回到按钮内会恢复，只有在按钮内松开才返回 `clicked`。动作仍由 App 处理，渲染可选
`DrawTextButton()` 或 `DrawBitmapButton()`。文本按钮默认叠加同一矩形上的半透明黑色蒙版；位图按钮
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

完整规则见
[Guest–Host Service ABI 稳定与演进规范](../../docs/design/guest-host-service-abi.zh-CN.md)。

Bundle 资源清单使用语义名称，不允许 App 手写 TOC 数字 ID。Bundle builder 对名称做唯一性、
格式、动画序列连续性和 32 位 ID 冲突校验。prepare 阶段通过同一次清单解析原子生成包含
`AssetId` 及可选 atlas 布局的 C++ 绑定和不可变 `resources.pack`；字符串和名称查找不会进入 Guest 运行时或
Resource Service ABI。例如清单中的 `button.start` 和
`food.normal.00..15` 会生成 `snake_assets::button_start` 与定长的
`snake_assets::food_normal[]`。资源重排不会改变由名称产生的内部 ID，名称拼写或帧数不一致则在
生成或编译阶段失败。资源清单及其引用的最终资源属于 App 源码并纳入版本控制；资源制作工具不参与
App 的 production 构建。资源 pack 携带版本、launch ID、逐项内容 hash 和整个目录的 SHA-256；
finalize 阶段只读取 AOT 和 pack，不会再次读取资源清单或重新分配 ID：

首版 `launch_asset` 是 Host 专用的不透明封面，不经过 Guest ABI 或 Resource Service。资源清单使用
`png_to_raw_rgb888` 在构建期把 PNG 和显式 `background` 合成为 raw RGB888；Host 从 Bundle 的
只读 Flash mapping 直接绘制，不进行运行时解码，也不保留封面 PSRAM 副本。

多帧动画应优先打包为 sprite sheet/texture atlas。Guest 只加载一次 Bitmap，再通过
`CommandBuffer::DrawBitmapRegion(x, y, bitmap, source_rect)` 选择帧；切帧只更新 command 中的
source rect，不触发资源查找、图片解码或 Bitmap 分配。

```sh
python3 tools/build_app_bundle.py \
    --app-manifest path/to/app.json \
    --asset-manifest path/to/assets/manifest.json \
    --prepare-resource-pack build/assets/resources.pack \
    --emit-cpp-header build/assets/app_assets.hpp \
    --cpp-namespace app_assets
# 使用 app_assets.hpp 编译 Guest 后：
python3 tools/build_app_bundle.py \
    --aot build/guest-p4/app.aot \
    --app-manifest path/to/app.json \
    --resource-pack build/assets/resources.pack \
    --output build/bundles/app.bundle.bin
```

生成源码后，P4 的单应用开发构建入口只需要源码路径。Bundle builder 会加入稳定 AppId、AOT、
资源 TOC 并补齐到 64 KiB extent：

```sh
bash tools/build_guest_app_p4.sh path/to/app.cpp
python3 tools/build_app_bundle.py --aot build/guest-p4/app.aot \
    --app-id vendor.app --output build/bundles/app.bundle.bin
bash tools/flash_guest_p4.sh /dev/cu.usbmodemPORT build/bundles/app.bundle.bin
```

默认在 `build/guest-p4/` 产生同名 `.wasm` 和 `.aot`，最终可烧录产物只有 App Bundle。构建脚本
固定 Restricted C++23、警告即错误、共享 Wasm memory 和 AOT 回跳中断点。开发 profile 使用
`-O1 -g` 并生成 AOT 调用栈；production `release` profile 使用 Clang `-O2` 和 WAMR AOT
opt level 3。只有明确以体积优先时才使用 `MICROPIXEL_GUEST_PROFILE=size`（Clang `-Oz`）：

```sh
MICROPIXEL_GUEST_PROFILE=release \
    bash tools/build_guest_app_p4.sh path/to/app.cpp
```

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
能采取不同动作”的接口才返回它们。当前 `KVStore` 的 key 不存在、配额不足和写入限流属于这类
业务分支；未来还包括网络失败、权限被拒绝或异步操作取消。Core Timer/Graphics 等不可恢复
Runtime 错误仍在发生点 panic。

`Result<T>` 是 freestanding Guest 对 `std::expected<T, Error>` 的兼容子集。应用使用
`has_value()`/`operator bool()`、`operator*`、`operator->`、`value()`、`error()` 和 `value_or()`；
失败值可由 `unexpected(Error{...})` 构造。Guest 关闭异常，因此错误状态读取 value 或成功状态
读取 error 会 trap，而不是抛出 `std::bad_expected_access`。完整标准库兼容不是目标，组合接口
只在实际 API 需要时增加。

## Watchdog 语义

1 秒 watchdog 是“连续 Guest 计算预算”，不是 `main()` 的总寿命：

- 阻塞在 `WaitEvent()` 时暂停；
- 每次进入 Host ABI 时重新计时；
- AOT 在循环回跳处检查异步终止标志，因此没有 Host 调用的死循环也能可靠停止；
- timeout、trap 和非零退出都会由 Host 清理资源并报告 failure。

因此正常事件应用可以永久运行，单次 callback 或两次 Host 调用之间的纯计算不能无限占用 CPU。

## 设计边界

- `Application` 是可发现的 capability façade；高层提供 `After/Every/Run`，accessor 按值返回
  copyable Service View，高级层提供阻塞事件读取；
- Service View 没有独立资源身份；Service 创建的 Resource 才以 move-only RAII 表达 Host
  handle 所有权；
- Public API 不把 C ABI 的失败模型逐行泄漏给普通应用；
- `Timer` 是 move-only RAII proxy，析构自动释放 Host 资源；
- `Event` 和 `TimerEvent` 是 value type，不暴露 raw source、sequence、handle 或 wire buffer；
- `guest/runtime/sdk.cpp` 集中执行 C ABI lowering 和错误码映射；
- `guest/runtime/startup.cpp` 在 C++ 构造器和应用 `main()` 前检查核心 ABI；
- capability 不能只靠 C++ 构造权限保证安全，Host 仍须验证 handle、类型、generation 和所属
  Guest。

高级接口的 `TimerFrom()`、`touch()` 和 `ResourceFrom()` 返回当前 `Event` 内 typed payload 的
受限 view；指针不得保存到该 `Event` 生命周期之外。48-byte wire event 只在 Runtime 中按
`service_id + event_id` 解码，不把 raw tag/payload 暴露给应用。

## Core 定稿边界

v1 已确定 `Application` façade、typed Service View、move-only Resource、Value/Event 和
`After/Every/Run` 控制流。底层收敛为七个 Core imports、按 Service 版本协商、48-byte Event、
`service_call` 控制面和 `service_submit` 数据面。后续能力应沿用这些对象语义和错误策略，不能重新
把叶子操作或 ABI 状态码堆回 `Application`。

尚未定义的是 Network、Camera、长音频/压缩音频的具体 method/channel/resource 组合，以及是否在
Public 类型稳定后引入 Typed IDL/binding generator；它们不改变 v1 transport。

格式、命名、所有权和 Guest 限制见
[C/C++ 代码风格](../../docs/development/code-style.zh-CN.md)。底层协议见
[Runtime Host ABI](../abi/README.md)。
