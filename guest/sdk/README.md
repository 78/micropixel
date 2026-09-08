# Guest C++ SDK

正式版前的 Scene、2.5D 前端与统一帧 API 迁移设计见
[SDK API 重构](../../docs/design/sdk-api.zh-CN.md)。该文描述目标接口；本文仍说明当前已实现行为。

SDK 让应用通过强类型对象使用图形、输入、音频和设备能力。应用保存自己的状态，以单线程事件循环
驱动更新；Host 管理硬件、资源和系统 UI。本文介绍编程模型与易错边界，完整可运行用法见
[Demo](../apps/sdk-demo/)，底层协议见 [ABI](../abi/README.md)。

## 工具链兼容性

当前 SDK 版本为 0.15.1，使用受限 C++23 和固定 commit 的
[MicroPixel WAMR fork](https://github.com/78/wasm-micro-runtime)
`af07c787ac6f7d1d20555f97ddc184f5fc13731a`，生成 AOT format v6。
wamrc 自报版本不足以判断兼容性。构建、打包和目标架构选择统一使用
[Guest 构建流程](../README.md)，不自行拼接编译命令。

## 最小应用

普通应用包含 `sdk/micropixel.hpp`，实现标准无参 `int main()`：

```cpp
#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_s;

int main() {
    micropixel::Application app;
    micropixel::Timer timer = app.timers().Every(1_s).value();
    app.Run([&](const micropixel::Event& event) {
        if (const auto* tick = event.TimerFrom(timer)) {
            (void)tick->delta();
            app.log().Info("tick");
        }
    });
    return 0;
}
```

`Run()` 是 Guest 自己的串行事件循环。Timer、输入和完成事件进入同一个 handler，不创建 Guest 线程，
也不会在 handler 中间插入另一个事件。返回 `EventResult::kExit` 可主动结束应用，返回 void 则继续。
高级 `WaitEvent/WaitEventFor/PollEvent` 用于短期等待或协议控制，不作为另一套常规应用模板。

## 对象与所有权

| 对象 | 语义 | 使用原则 |
|---|---|---|
| Application | 能力入口与事件循环 | 通过 accessor 取得 Service，不堆积叶子操作 |
| Service View | 可复制的能力入口，如 Audio、Renderer | 本身没有独立资源身份 |
| Resource | Timer、Texture、Sensor、Playback 等 | move-only RAII；释放后不得继续使用旧身份 |
| Value / Event | 时间、坐标、事件数据 | 值语义，typed payload view 不超过 Event 寿命 |

保存 Service View 不会自动延长其创建资源的寿命。资源的 Guest 所有权与 Host 的在用引用可能独立：
Scene 引用 Texture，播放实例引用 AudioClip；释放 Guest 句柄不会让仍在使用的底层内容立即失效。
跨 ABI 的身份、所属应用和容量仍由 Host 校验，C++ 类型不能代替隔离检查。

## 时间与事件

使用 `Duration` 表达间隔、`TimePoint` 表达应用时钟上的时间点，单位写成 `16_ms`、`1_s` 或显式工厂。
时间运算的溢出、下溢和除零会 trap。应用时钟在暂停时冻结，不能与 Host wall clock 混用。

Timer 由 `app.timers().After/Every()` 返回的 `Result<Timer>` 创建，通过 `event.TimerFrom(timer)` 匹配来源。
周期通知合并时，`delta()` 累加实际经过时间，`missed_count()` 表示未单独投递的 tick 数。
`Cancel()` 返回 `Result<void>`，成功后释放 handle，失败时保留所有权供重试；需要再次调度时创建新 Timer。Reset 和析构只做 best-effort 释放。

Touch position 与 Renderer 共用逻辑坐标；pressure 只有在 capability 声明支持时才有效。
Key 使用方向、Confirm/Back/Menu 与按位置命名的 South/East/West/North，不依赖手柄上的 A/B/X/Y 标签。
系统手势由 Host 处理，应用不要重新实现系统菜单或返回大厅的手势。

## Resume、Stop 与 watchdog

暂停冻结应用时钟、Timer、输入和音频。恢复同一 Session 首先收到 Resume，不重新调用 main；
Host 先显示保留画面，需要重建动态内容的应用可再重绘。Guest 没有 Pause 事件。

关闭或切换应用时投递 Stop，handler 返回后 Run 返回；500ms 内未结束才强制终止。
保存状态应在这一有界时间内完成，不能依赖析构执行长操作。

1 秒 watchdog 限制连续 Guest 计算：阻塞等待事件时暂停，进入 Host ABI 时重新计时，AOT 回跳点
检查终止标志。应用可以长期运行，但 handler 或纯计算循环不能无限占用 CPU。

## 图形：先选择更新模型

| 场景 | 模型 | 原因 |
|---|---|---|
| 页面、精灵、对象移动 | Scene | 保留对象，仅传递变化属性 |
| raycaster 等整帧光栅 | HostSurface + RasterResources | 批量绘制到 Host buffer，减少像素传输 |
| 内核表达不了的逐像素自绘 | GuestSurface | Guest 直接写线性内存中的整帧像素 |

### Scene 与布局

`Renderer::CreateScene()` 返回 `Result<Scene>`；可以保留多个场景供页面切换。
Container 是子树所有权边界和局部坐标空间，子对象的位置相对直接父 Container，
visibility、opacity、translation 和 clip 沿父链生效。

直接修改节点后调用 `renderer.Present(scene)`。setter 只修改 Guest 状态；提交失败保留旧画面和
待提交变化。节点销毁立即失效，失败不恢复已销毁 handle，槽位复用也不能让旧 handle 指向新对象。
首次、场景切换和结构变化提交 keyframe；其他帧合并净变化。旧 `Scene::Update` 已移除。
基础节点工厂返回 `Result<节点类型>`；容量不足时不会创建半个节点。具体剩余工作见 [SDK 重构状态](../../docs/design/sdk-api.zh-CN.md)。

布局依据 RendererInfo 的逻辑 width/height 与 safe area。SDK 使用短边 720 的逻辑画布，序列化时统一
转换为物理值；physical width/height 用于物理素材选择等明确需要原生像素的场景。Touch 属于 Scene
坐标，跨 Container 使用 ToLocal/ToScene，高层控件自动转换。

Sprite 适合独立图像，SpriteBatch 适合蛇身、方块和粒子；Shape/RoundedRect 保存形状属性，不各自
分配像素 surface。Label 使用 Small/Medium/Large/Title 语义字体，具体字号由 Host profile 决定。
[symbols.hpp](symbols.hpp)提供保证存在于系统字体的图标。

Scene 节点、容器和 Batch 实例没有固定计数上限：Host 按实际提交需求扩容，App 无需声明或预留
Host 容量；wire id 为 uint16，节点数加实例数不超过 65535（Batch 本身仍是一个节点），其余由内存决定。
创建失败返回 `kResourceExhausted`，Host 端资源不足时提交失败并保留旧场景，挂起保留缓冲，退出释放。
Label 文本存放在按需增长的 text arena 中，单条文本上限由 Host 报告（当前 1024 字节）；`SetText`
在 Guest 内存耗尽时 panic。Guest 存储按实际工作集增长；页面和 Batch 的槽位可以复用，不能把动态
容器理解为无限资源。

`cache_content` 是 Host 渲染提示，当前用于选择根级 Layer 快照容器，适合内容不变的整体平移。
它不保证任意子树缓存，也不应被当作影响画面语义的 API；缓存行为与诊断见
[Graphics 性能文档](../../docs/development/graphics-performance.zh-CN.md)。

### Texture 与 atlas

使用生成的 AssetId 加载资源，不手写 TOC 数字。`LoadTexture(asset)` 保留素材像素尺寸，
`LoadTexture(asset, TextureScale::kDisplay)` 显式适配 2D 逻辑画布。
`resources.CreateDynamicTexture(size, format, pixels, pitch)` 返回同一种 `Result<Texture>`。
`texture.Update(rect, pixels, pitch)` 准备完整的新像素版本，下一次 Present 生效；失败保留旧像素。
已有节点和材质引用自动跟随更新，不需要每次重新绑定。普通素材 Texture 的 Update 返回 Unsupported。
ABI 2.0 没有 StreamingTexture / TextureUpdateBatch，动态纹理是唯一的可变纹理路径。

Scene 已接受的快照独立保留纹理引用；Guest 销毁 Texture 不破坏已显示的画面。

动画优先使用 atlas：加载一次、逐帧改变 source rect。时间由应用事件循环驱动，当前没有
AnimationClip/Track。资源清单、生成绑定与 Bundle 工作流见 [Guest 构建](../README.md)。

### Raycaster（2.5D 前端）

`sdk/raycast.hpp` 的 `Raycaster` 在 Guest 内完成栅格世界的几何：射线 DDA、墙与门板投影、
地板/天花板行设置、覆盖裁剪、billboard 深度排序与逐列 z-test，输出 `RasterDrawList` 的
`SpanPair` / `Column` 记录；每像素工作由 Host 内核执行，不新增 ABI。App 维护一张行主序的
`RaycastCell` 数组（4 字节/格：`Empty / Wall(slot) / Slab(slot, open)`），以 `RaycastGrid` 视图交给
`Cast(camera, grid)`，墙或门变化时改写对应格子；每帧调用 `Cast`、`DrawWorld(list)`、
`DrawBillboards(list, billboards)`，之后用普通 RasterDrawList 方法追加武器、HUD 和文本。
`Depth(column)`、`LightFor(distance)`、`Project(x, y)` 供 App 做可见性和瞄准判断。
所有状态为固定容量数组（`kMaxColumns`、`kMaxBillboards`），`Initialize` 后不再分配。

`sdk/raster_world.hpp` 放 2.5D 前端共享的 `DistanceLighting` / `LightTable` 与 `Billboard`，
后续的 Mode-7 地面（赛车）与球面视图（earth）复用同一套光照与 billboard 约定。
maze-evil 的 `game/renderer.cpp` 是当前的完整用法。

### DirectSurface：HostSurface 与 GuestSurface

整帧路径有两种 surface，区别是像素归谁：`HostSurface` 的 buffer 由 Host 持有，Guest 只提交绘制记录；
`GuestSurface` 的 buffer 在 Guest 线性内存里，App 自己写像素。两者共享基类 `DirectSurface` 的帧背压接口
（`Busy` / `AcquireFree` / `Present` / `ReleasedFrom`、尺寸与 `direct_scanout` 查询），但绘制入口是类型级的：
只有 `HostSurface` 有 `Update()`，只有 `GuestSurface` 有 `Buffer()`。一个 App 同时只能有一个 surface，
`CreateHostSurface()` / `CreateGuestSurface()` 都会因已有 surface 而失败。

`HostSurface` 通过 `surface.Update(buffer_index, callback)` 开始绘制，回调中的 `RasterDrawList&` 自动绑定该 buffer，
回调返回时自动完成提交。回调签名为 `void(RasterDrawList&)`，列表不能复制或移动，不能手动结束。
目标无效时不调用回调；绘制失败由 `Update()` 的 `Result<void>` 返回。缓冲区满时会分批提交，
因此回调不是事务，失败不回滚已绘制的批次；仅成功后调用 `Present()`。

`RasterResources` 负责上传 INDEX8 纹理、canonical RGB565 调色板和 warp 表。它归 Session 而不是某个 surface：
销毁 `HostSurface` 切到 Scene 再重建，已上传的资源仍然可用，不需要重传；App 结束时统一释放。
Guest 决定几何、遮挡和顺序，Host 执行逐像素操作。
完整调用签名见 [graphics.hpp](graphics.hpp)，可运行示例见 [Maze Evil](../apps/maze-evil/)。

帧的生命周期是“取得空闲 buffer → 绘制 → Present → Host 归还”：

- Present 成功后 buffer 归 Host；Busy 时不能改写、绘制或重复 Present。
- 所有 buffer 忙时等待 ReleasedFrom 事件，不能忙循环抢占 CPU。Host 可保留当前显示帧直到下一帧替换，
  连续提交 N 帧不保证立即得到 N 次释放事件；销毁归还最后一帧，不再投递其事件。
- 暂停时 Host 停止扫描输出并归还在飞 buffer，恢复后继续 Present。
- DirectSurface 存活期间拒绝 Scene submit。系统 UI 仍归 Host，必要时退回合成；direct_scanout 为假
  时接口语义不变。max_full_frame_fps 是传输上限，不是应用可达到的保证值。
- buffer 可按整数 upscale 缩小，代价是放大处理与画质变化，必须测量最终呈现时间。

`GuestSurface` 供需要直接写像素的应用使用：Bundle 必须声明 `pinned_memory: true`，保持线性内存
基址不移动，否则创建返回 Unsupported。通过 `Buffer(index)` 取得行主序 RGB565 像素，`pitch()` 为行字节数，
像素按面板字节序写入，依据 rgb565_byte_swapped 查询；写之前先确认 `Busy(index)` 为假。
这种模式会提前保留连续内存；`HostSurface` 无需此声明，Guest 内存按需增长。

RasterDrawList 的 Column 使用列主序纹理，SpanPair 与 Warp 使用行主序；Sprite/SolidSprite 用于图像和字形，
FillRect 用于填充或混合。Column/SpanPair 的坐标由调用方预先裁剪，Sprite/FillRect/Warp 的目标由 Host 裁剪。
调色板按槽上传（`UploadLitPalette(slot, ...)`），`SetPalette(slot)` 之后的记录用该槽，一个 App 可以为地表、
每种精灵和 UI 各留一套调色板。Warp 是 Host 持有的 screen→(u, v, light) 表（`UploadWarpMap` /
`UpdateWarpRows` 分帧流式上传），每帧只提交一条记录和 `u_offset / v_offset`（`u_fraction_bits` 让 u 带纹素小数，
贴图可亚纹素滚动）；`SphereView` 用它生成球体
（本地基准 Earth Garden，未入库），任何视点固定、贴图滚动的映射（天空盒、隧道、水面）都适用。
纹理宽高使用 16 位字段（1–65535），槽编号保持 8 位（0–255），可重复上传到同一槽替换纹理。
`texture_slot` 表示槽编号，`light_level` 表示调色板光照档位索引。INDEX8 每像素 1 字节，实际上传受
可用内存约束，源数据必须完整位于 Guest 内存。建议性能敏感的纹理优先采用 2 的幂尺寸
（如 512×256），以使用移位和掩码采样快速路径；300×200 等尺寸也支持。
上传分配失败返回 ResourceExhausted，并保留原纹理或调色板。元数据按需分配在 PSRAM，
App 结束时释放所有纹理、调色板和元数据。替换期间新旧资源同时存在，需要临时容纳两者。

每个提交批次先验证再写像素。SDK 缓冲满时会自动分批，Finish 返回首个错误；此前已经成功的批次
不会整体回滚。应用只应在绘制成功后 Present。关闭 Host raster 能力时返回 Unsupported，应用需
明确选择 Scene 或 Guest buffer 回退，不能假定 Host buffer 总能绘制。

### 组合控件

普通页面优先用 Flex/Grid 容器描述布局，使用 TextButton 或 ImageButton 组合显示与点击行为。
完全定制的按钮可用无堆分配的 `ui::Button`，它捕获 touch id，移出取消按下视觉，移回恢复，内部松开才
触发 click；hit padding 扩大触控区但不改变画面，相邻目标不应重叠。

文字按钮默认居中裁剪溢出文字，并提供 text_clipped 与一次诊断 warning；需要严格拒绝时显式使用
TextOverflow::kReject。后续修改失败不能提交一半属性。控件 ToString 可用于错误诊断，具体属性与
限制见 [ui](ui/)。

## 音频

Tone 用于短音效；AudioClip 表示资源，Playback 表示一次播放，可暂停、恢复和停止。
Host 在播放期间独立 pin clip，释放 Guest clip 不打断已开始的播放。音效只设置单次 volume_per_mille，
设备主音量始终由 Host 管理；游戏参数源与验收见
[音频规范](../../docs/development/game-audio.zh-CN.md)。

Ogg Opus 由 Host 解码和缓冲，Guest 不访问 codec/I2S。采样率与可用播放容量从 AudioInfo 查询，
不按板名硬编码。PcmStream 适合应用自己合成音频：Write 返回实际接收的交织 int16 帧数，短写表示
环满，等待 LowWaterFrom 后继续；欠载播放静音而不结束流。

每个应用最多一条 PCM stream，支持 1/2 声道，采样率为设备混音率或其整数分频。Close、析构或
StopAll 关闭流；暂停期间保留流，恢复后继续播放已缓冲数据。接口见 [audio.hpp](audio.hpp)。

## 设备发现、传感器与 GPIO

设备目录回答“有什么”，具体 Service 负责操作。不透明 DeviceId 不等于枚举位置，parent 表达组合
设备关系，应用依 kind/capability 选择设备，不根据物理名称推导路由。

Sensor 按 Acceleration、AngularVelocity、MagneticField 等 reading 类型打开，单位由类型表达。
Open 才启动采样，SetSampleInterval 在设备范围内配置频率，Read 读取缓存而不等待 I²C 转换；刚打开、
改频或恢复后的首个周期可返回 WouldBlock。最后一个 handle 释放或应用暂停后停止采样。

GPIO 打开即租用板级白名单中的引脚，释放后恢复安全输入状态。edge input 使用 EdgeFrom 接收变化；
未订阅边沿时主动 Read。PWM duty 和 Haptics strength 使用 0..1000，持续时间使用 Duration。
应用不能打开系统已占用的引脚。具体接口见 [SDK 头文件](./)和 [Demo 设备页](../apps/sdk-demo/pages/)。

## 存储、启动参数与语言

Package 资源与应用私有 KV 存储是独立入口，不暴露文件系统路径。GetBytesSize 先查询精确大小，
再分配 buffer 并 GetBytes；key/value 上限由 KVStore 常量给出，UTF-8 key 按 bytes 计数。
Random::Below 使用无偏范围采样，需要范围随机数时不要自行对 U32 取模。

CLI 的 `--` 后参数属于本次新建 Session，应用从 launch_arguments 读取，FindValue 同时识别
`--level 100` 和 `--level=100`。Host 最多接受 16 项、合计 512 bytes（含 NUL）；暂停恢复不重新传参，
普通大厅启动参数为空。CurrentLocale 同样在 Session 启动时确定，系统语言变更在下次启动生效。

## Guest STL profile

Guest 使用 wasi-sdk 33 的 no-exception libc++。已验证子集包括 array/span/string_view、optional/variant、
常用 algorithm，以及 string/vector/map/queue/deque、unique_ptr 和动态分配。不使用的代码由链接器 GC
移除。应用内部可以使用这些容器，Public ABI 不暴露 STL 布局。

线性内存同时容纳静态数据、辅助栈和动态 heap，按需增长，当前 Host 策略上限最多 8 MiB；实际值由
连续 PSRAM 与 Host 安全水位决定。Host-owned Texture/surface 另行分配，同样动态检查安全水位。
普通 new 的 OOM panic，nothrow new 返回 nullptr；长期所有权仍用容器或 RAII。

这不是 WASI/POSIX 环境。thread、mutex、filesystem、socket、locale/iostream 和系统调用不受支持；
exception、RTTI 与 reference-types 关闭，不能自行增加 WASI import。

## 错误策略与 Service 演进

能采取其他动作的业务失败返回 Result，例如资源缺失、解码失败或容量不足；调用方检查结果并选择
回退或带原因终止。事件等待等基础操作的编程或 Runtime 错误在发生点 panic，避免把
机械状态码检查扩散到应用。自定义不可恢复错误使用 Assert/Panic 并提供原因。

Result 提供 expected 风格的值/错误访问；读取错误状态的 value 或成功状态的 error 会 trap。
析构不 panic，只做 best-effort 释放。Host 捕获 Trap、记录诊断并清理 Session。

Public 方法不与 Wasm import 一一对应，SDK/Runtime 隐藏 service ID、wire 与 handle。
新能力通过 Service 版本和 capability 演进，应用对可选能力明确回退。当前 Bundle requirements 与
完整的启动前能力预检尚未实现，不能假定构造 Application 已检查应用全部需求。
Network、Camera 和网络资源加载尚未定义公开接口。

实现规则见 [代码风格](../../docs/development/code-style.zh-CN.md)，边界验收见
[conformance](../tests/conformance/)。
