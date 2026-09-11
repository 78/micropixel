# Graphics 性能诊断与基线

图形性能要沿着“应用更新 → 合成 → 呈现”分别测量。CPU 占用、Guest 提交速度和屏幕可见帧率回答的是
不同问题。本文保留测量方法、当前机制和回归标准；接口用法见 [Guest SDK](../../guest/sdk/README.md)，
wire 规则见 [ABI](../../guest/abi/README.md)。

启用 `CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG` 后，同步 blit 扫描路径每 60 秒输出
`scanout-timing`：`intervals` 为完成传输间隔数，`elapsed-us` 为窗口时间，`fps-milli`
为间隔数除以时间，`p95-upper-us` 为 1 ms 直方图的 P95 上界（0 表示无法给出有限上界）。
统计在 presenter 任务完成传输后更新，不在 ISR 或 Guest 逐帧输出。切换到独占扫描时重置窗口；
丢弃第一窗口作为预热，再记录连续三窗口。该指标是面板传输完成节奏，不是面板光学响应测量，
也不代表 P4 framebuffer flip 或 LVGL 合成路径；没有该日志的路径须另行测量。

## 1. 先确定正在走哪条路径

| 应用模型 | 像素如何产生 | 如何呈现 |
|---|---|---|
| Scene | SDK 提交净差量，Host 合成到 App Surface | 支持的板型可直接扫描输出，否则经 LVGL 合成 |
| HostSurface + RasterDrawList | Guest 提交绘制记录，Host kernel 写 Host buffer | Presenter 扫描输出，系统 UI 可见时退回合成 |
| GuestSurface | Guest 写线性内存中的整帧像素 | 同一 Presenter；需 pinned memory 保证地址稳定 |

Scene 路径可分为：

```text
Guest 逻辑与属性更新 → SDK 编码 → ABI 校验与 Scene 应用 → damage / 光栅化
    → App Surface 发布 → 直接扫描输出 或 LVGL 合成 → 面板传输 / 翻页
```

前半段通常发生在 Guest 的同步提交中，后半段由显示任务异步执行。Present 返回表示提交完成，
不表示面板已显示这一帧；不能把不同帧上的同步耗时与显示耗时直接相加。

P4 的 DPI framebuffer 翻页与 S31 的 QSPI 窗口传输成本不同。S3 使用 CPU compositor 和 SPI DMA；
SPI DMA 是传输能力，不代表拥有 PPA/DMA2D 像素加速。

## 2. 分段测量

固定 Host、Bundle、板型、分辨率、构建 profile、HUD、音频和输入场景，每次只改变一个变量。
使用 release 或明确记录的 performance profile，预热后聚合至少 120 帧。分别覆盖稳定移动、文字/粒子、
平移缓存和全屏变化；短时峰值与持续运行结果分开记录。

| 阶段 | 看什么 | 能回答的问题 |
|---|---|---|
| Guest 更新 | world/HUD 耗时、实际改动对象数 | 是否遍历或修改了大量未变化对象 |
| Guest Present | 同步调用耗时、wire bytes/records | 编码与跨 ABI 提交是否过重 |
| Host apply | 校验、资源 retain、Scene 应用耗时 | 是否在小 patch 上重复处理整个 Scene |
| Host normalize/damage | 展开数、区域数、合并次数 | 局部变化是否被扩大成全量工作 |
| Host render | 像素数、重放数、CPU/PPA/DMA2D 分布 | 瓶颈是计算、硬件启动还是内存带宽 |
| 发布与等待 | replaced、buffer release、锁等待 | 生产速度是否超过显示消费速度 |
| 显示 | LVGL refresh、panel bytes、copy/flip 时间 | 重复拷贝、传输或刷新调度是否限制帧率 |

Guest Present 减去 Host 同步处理时间，可近似估计 SDK/WAMR 边界成本；只比较聚合统计，不比较两端
单帧时间戳。DirectSurface 还应分开看 render、present 与等待空闲 buffer 的时间，以及
`scanned-out/composited` 比例。等待占主导时，继续优化光栅循环未必提高可见 FPS。

不要把每个 Scene submit 或 panel flush 都算一帧：多个提交可能被合并，一帧也可能拆成多个窗口传输。
性能 HUD 本身会改变合成路径，A/B 必须固定其状态，并核对 Guest 和 Host 两侧计数。

诊断文本应交给 `work::BackgroundExecutor` 异步输出，并受 `CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG`
控制。115200 波特率下同步输出约 1.5 KiB 文本就可能占用约 100ms，探针本身会制造尖峰。
临时逐帧计时用完移除，保留有界聚合遥测。

## 3. Scene：减少重复工作

Scene patch 缩小传输量，damage 缩小重绘量，两者必须分别验证。几百字节的 patch 仍可能触发整棵树
展开或整屏重绘；wire 很小不能证明 Host 很快。

当前 SDK 按事务净变化编码，Host 根据属性变化增量更新 normalized operation。结构变化、keyframe 和
需要重新排序的情况仍走完整校验路径。优化应先减少实际工作量，再考虑每条指令的成本：

- 复用 SpriteBatch 槽位，普通移动只改变头尾或真正变化的 instance。
- 避免逐帧重测未变化文字，页面布局在内容或尺寸变化时更新。
- 检查 damage 是否由容量合并、裁剪或历史缓冲追平扩大。
- 对平移且内容不变的子树使用 Layer 快照，避免重放内部对象。

`cache_content` 当前用于选择根级 Layer 快照容器，不承诺任意子树的持续局部缓存。
缓存需要额外像素内存和失效维护；已有的小 damage 路径可能比维护整视口缓存更便宜。

多 App Surface 允许 Guest 在不持 LVGL 锁时合成和发布。单槽 mailbox 可以替换尚未采用的帧，但必须
合并 damage；每块 surface 记录尚未补齐的变化，复用前先追平。单 surface 则需要锁内合成。
锁序与缓冲所有权见 [Guest graphics engine](../../firmware/espressif/main/platform/lvgl/guest_graphics_engine.hpp)。

## 4. 呈现：省掉拷贝，也要正确交接

系统 UI 不可见时，App Surface 可交给 Presenter，减少 LVGL 调度和重复合成。系统页面、对话框或转场
出现时，扫描输出仲裁器交回 LVGL，并完整同步当前画面。进入直接输出时同样需要初始化显示内容，
不能只沿用旧路径的局部 damage。

| 板型 | Scene 直接输出 | DirectSurface 输出 |
|---|---|---|
| Mosaico / S31 | RGB565 damage 按面板窗口对齐、换序后传输 | 原尺寸、面板字节序的 RGB565 可直接传输 |
| Claw4 / P4 | BGR888 damage 拷入空闲 DPI framebuffer，再翻页 | RGB565 经 PPA 转 RGB888 后翻页 |
| S3 preview | 保持 LVGL 路径 | 合成到 App Surface 后走 LVGL |

双 framebuffer 的空闲帧可能落后多次更新，必须补齐本次 damage、该 buffer 缺失的历史变化和旧 overlay
足迹。历史记录只保存每帧自身变化，不能把“为了追平而整帧拷贝”继续记成新一帧的 damage，否则以后
每帧都会退化为整帧。

HUD 与手势提示可由 Presenter 小区域叠加，避免一个小蒙层迫使整帧走 LVGL。若临时覆盖 Guest buffer，
Host 必须在归还前恢复原像素。截图必须读取当前显示来源，不能读取已经停止更新的 LVGL shadow。
暂停后到安全点前到达的帧也必须保持暂停语义，不能重新接管面板。

实现入口：[Presenter](../../firmware/espressif/main/platform/lvgl/display/direct_surface_presenter.hpp)、
[扫描输出仲裁](../../firmware/espressif/main/platform/lvgl/display/scanout_arbiter.hpp)、
[共享 stage pool](../../firmware/espressif/main/platform/lvgl/display/scanout_stage_pool.hpp)。

## 5. 像素格式、加速器与内存

RGB565 比三字节颜色少占三分之一像素空间，但只有整条路径支持它才有收益。资源、合成目标、缩放、
透明混合和面板提交要一起检查；仅改目标格式可能让原本的 DMA2D/PPA 操作全部落到 CPU 转换。
透明资源保留 alpha，不能为节省带宽静默转成不透明 RGB565。

PPA/DMA2D 有启动、描述符和 cache 同步成本，小操作可能比 CPU 更慢。门槛应由 fill/blend 面积直方图和
真机 A/B 决定，不能把一种板型上的阈值直接套到另一种板型。当前常量在
[像素 compositor](../../firmware/espressif/main/platform/graphics/esp_pixel_compositor.cpp)维护，本文不复制数值。

硬件源地址同样影响资格与性能。Raw 资源加载时优先暂存到 PSRAM，避免热路径反复从 flash 映射读取。
检查 CPU fallback 时同时看格式、对齐、源内存类型和操作面积。S31 内部 SRAM 不经 cache，对地址做
cache 同步前必须检查 line size，为零时跳过。

整帧 buffer、截图和转场共享 PSRAM，空闲总量足够也可能没有足够大的连续块。阶段性互斥用途优先借用
预分配 stage pool，并明确定义借用与归还时机；不要让一次动画必须成功依赖运行期大块分配。
Guest buffer 的 pinned memory 会提前占用连续空间，默认 Host buffer 则允许 Guest 线性内存按需增长。

返回大厅时，如果板级截图阶段已完成缩小动画，Hall 直接接管截图卡片，不再重建转场背景。
排查 `Hall background region update failed: captured=no` 时检查 LVGL 自动行对齐：截图缓冲区按
`lv_draw_buf_width_to_stride` 分配，交给只接受紧密 RGB888 的拷贝路径前去除行填充。
`lcd.dsi` 的 underrun 则表示扫描输出取数不足，应结合 PSRAM 带宽与真机画面单独验证。
Claw4 的 DPI 像素时钟设为 40 MHz，由默认 240 MHz 时钟源精确 6 分频产生；
按 720×720 和现有消隐时序估算约 65.5 Hz。
相对原来的 48 MHz，扫描像素带宽需求降低约 16.7%；RGB888、DSI lane 速率保持不变。
P4 L2 Cache 配置为 256 KiB、cache line 为 64 B；相对 128 KiB Cache 额外占用 128 KiB 内部 SRAM。
ESP-Hosted transport 缓冲池优先放在 PSRAM，以保留内部 SRAM。当前发送池的 1600 B 块间距能满足
64 B 对齐，但不能保证每块都满足 128 B 对齐；不要在该配置下单独将 cache line 改回 128 B，否则
SDIO DMA 参数检查可能返回 `ESP_ERR_INVALID_ARG`（258），继而触发 Host 重启。
P4 的 LVGL 图像、App Surface、转场、扫描暂存池及对齐 bitmap 分配跟随生成配置中的 cache line
大小，当前为 64 B；DMA 目标分配长度仍覆盖完整 cache line。PPA/DMA 的 128 B burst 长度独立于
分配对齐，不随本次 cache-line 配置调整。
调整后需真机检查内部 heap 最低余量及转场、应用运行时的 underrun，编译通过不能证明运行余量充足。

保留截图作为卡片图片时，不要把 LVGL 绘制缓冲区的行对齐要求套在图片源上。P4 的 202×202 PPA
截图每行 606 字节，而解码缓存每行按 LVGL 配置对齐。两者都可通过图片 descriptor 的显式 stride
直接显示。封面缓存和板级 native-cover 判断共用 `CanUseSourceDirectly`；只有尺寸或格式需要转换时
才排队处理，避免已完成的截图在返回大厅时短暂退回纯色和标题占位封面。

## 6. 全屏光栅：降低跨边界与像素成本

HostSurface 的 RasterDrawList 把 Column、Span/SpanPair、Sprite/Image、Rect、Text 等批量记录交给 Host 执行。Guest 保留光线投射、
遮挡判断和绘制顺序，Host 校验记录后写入空闲的 Host buffer。这样既摊薄跨 ABI 成本，也避免 Guest
逐像素循环的地址计算和边界检查开销；Host kernel 仍可能受 PSRAM 带宽限制。

S31/Mosaico 480×480 上的实测（coastline 迁移，`raster kinds` 遥测）：不缩放的不透明 Image 拷贝约 65–70 ns/px，
Span 约 50 ns/px，不透明 Rect 约 60 ns/px，都接近 PSRAM 读+写的内存瓶颈；Host buffer 全屏写一遍约 6 ms。
把内核改成 32 位字操作、按字换字节序，只带来约 5% 改善。因此第一优先级仍是减少被覆盖的像素
（背景只画路面两侧、HUD 下不画路面），其次才是内核细节；关闭 Guest AOT 边界检查
（`--unchecked-memory`）在 coastline 每帧约 40 ms 中只省约 1.3 ms，说明瓶颈不在 Guest 侧。
HostSurface 帧由 Guest 同步提交，一帧超过 30 FPS 槽位时不要按周期网格跳到 15 FPS，应在完成后的下一
Tick 续帧，并用三个 buffer 避免前一帧扫描未释放导致的空转（coastline 由此从 15 FPS 提到 24 FPS）。

S31 上用 `Dma2dCopyEngine` 做 PSRAM→PSRAM 的 RGB565 裸拷贝：512×480 单块约 12 ns/px（3.2 ms），
480×150 单块约 13 ns/px，而 32 个 100×4 窄条一次事务约 39 ns/px，与 CPU 换序拷贝（空载 39 ns/px）
持平。所以 Raster 的 Image 硬件路径只接受裁剪后宽 ≥32、面积 ≥4096 的块，且全屏背景应作为一条
记录提交，而不是按路面两侧切条。DMA2D 的 TX scrambler 按 3 字节组置换，不能给 2 字节像素换字节序
（实测输出错位并在下一事务超时），因此纹理必须预先按面板字节序保存（`kRgb565ByteSwapped`），
硬件只做同序拷贝。接入 coastline 后 S31 `--demo`：Host 内核 27 → 21.5 ms/帧（`raster copy engine` 每帧
1 批 1 块 3.9 ms、16 ns/px 含 cache 同步与唤醒），渲染 34.8 → 29.5 ms，24 → 28 FPS；剩余 Image 时间是
带 alpha 的 BGRA 赛车/图标，仍走 CPU。

硬件路径要求纹理与目标同格式、同比例。P4/Claw4 的面板是 RGB888，`ResourceService` 默认把不透明纹理
解码成 BGR888；同时 coastline 的 HostSurface 是 720/2 = 360×360 buffer，而 `TextureScale::kDisplay`
按面板比例解码，背景对 buffer 是 2:1。两者叠加使整屏 Image 落到逐像素转换采样（约 26 ms、195 ns/px），
比原先按路面切条时更慢。修正：建立 DirectSurface 后 `ResourceService` 改按 surface 的 RGB565 格式解码
之后的不透明纹理；SDK 新增 `TextureScale::kSurface`（面板比例除以 surface upscale）。P4 `--demo`：
Host 内核 36 → 13.3 ms/帧（Image 26 → 4.4 ms，DMA 背景块 2.4 ms、18 ns/px），渲染 38 → 19 ms，
22.5 → 29.6 FPS。检查 `micropixel_resource: loaded format=… WxH bytes=…`：bytes 应为 W×H×2。

先减少被覆盖的像素写入，例如只画墙面未覆盖的地板，再比较 kernel 本身。纹理布局应匹配读取方向，
按列纹理的顺序读与目标按行写之间存在取舍，单纯改变目标遍历顺序可能得不偿失。

整数缩小渲染能显著减少像素数，但 Host 放大、换序和面板传输仍有成本，不能直接把像素减少比例当作
FPS 提升。P4 高分辨率场景与 S31 原尺寸输出应分别测量。

不再维护已删除的 Fast memory/shared-heap 实验接口。该实验说明：改变数据驻留位置也可能改变所有
访存的代码生成成本，必须用对照实验分离变量；不能仅凭查表耗时推断 cache miss 是主因。

## 7. 回归与基线维护

每条性能基线必须带板型、Host/Bundle 版本、构建 profile、分辨率/缩放、HUD/音频状态、场景和采样窗口。
链路变更后重新测量并替换旧基线。旧文档中不同阶段的 FPS 和 A/B 表不再作为当前性能承诺；本次文档
整理未重新测量真机，因此不补写新的性能数字。

Scene 真机回归使用 Snake；存在本地 Mario 时可增加滚屏场景，但它被 gitignore 排除，不作为仓库必需
依赖。使用其 `--benchmark --no-bgm` 参数，长跑通过 `micropixel run --no-follow` 启动后按需读取日志，
具体连接和命令见 [烧录指南](flashing.zh-CN.md)。

Snake 在 480×480 上保留以下验收约束：首次 keyframe 可全屏；稳定普通移动单次 wire 不超过 16 个
changed instances、damage 小于屏幕 10%、`capacity-merges=0`；atlas 切帧为单节点 patch；震动应出现
Layer cache 和 translation-only wire，重绘区域小于全屏。面板窗口路径核对提交与 shadow copy 的次数、
像素数；直接输出路径则按其实际 copy/flip 边界检查，不能混用不同路径计数。

共享 graphics/LVGL/PPA 分支修改需构建 P4、S31 和至少一款 S3，并运行相关 Host test。真机还应覆盖：
系统 UI 接管与退出、暂停恢复、截图、在飞 buffer 保护、颜色/字节序、音频并发和连续内存占用。
性能正确不能代替生命周期和画面正确性。

仍需关注的限制：

- Mosaico 截图有三个来源：LVGL 合成时读 displayed shadow；Scene 直接输出读正在扫描的 App Surface；
  Direct Surface（HostSurface / Guest buffers）独占扫描时由 presenter 任务把 front buffer 拷成一帧
  面板尺寸的 RGB565 再编码（`DirectSurfacePresenter::CaptureFront`），拷贝期间 front 不会被释放。
  修改截图时应分别验证这三种模式，不能由一条通过推断另一条通过。
- 状态层的大块对话框快照在 PSRAM 紧张时可能退化为无动画。
- Claw4 Scene 直接输出仍需 App Surface 到 DPI framebuffer 的拷贝；进一步消除它需要重新设计
  buffer 借用与 LVGL 交接，不能仅删除 copy。

## 日常日志与详细启动采样

默认保留每个 App 首个 scene 的完整性能报告，以及每 600 个 scene frame 的周期报告、
每 300 次 display refresh 的统计和 layer-cache 状态切换。启动时不再连续输出八组累计统计。
排查启动前几帧时，将 `CONFIG_MICROPIXEL_APP_SURFACE_STARTUP_TELEMETRY_FRAMES` 设置为 `8`
后重建 Host；默认值为 `1`。`CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG` 仍控制整组图形遥测。
这些累计硬件计数可能包含之前的 App，比较连续采样的差值，不要把首帧累计值当成本 App 耗时。

生命周期内存记录合并为一行，分别列出 SRAM/PSRAM 的 `total/free/min/largest`，单位都是字节；
`min` 仍是自启动以来的最低空闲值。每个加载阶段的完成耗时和错误保留在 INFO，阶段开始标记及
重复的 PNG 解码地址信息移到 DEBUG；资源服务的加载结果、尺寸和耗时仍在 INFO。
需要 DEBUG 细节时，在专用 sdkconfig defaults 中设置 `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y`、
`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`，使用独立 build 目录构建，避免旧 sdkconfig 覆盖 defaults。
错误、警告、触摸延迟、转场耗时及资源清理计数不受本次压缩影响。
