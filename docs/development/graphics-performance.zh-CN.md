# Graphics 性能诊断与基线

本文记录 retained Scene 图形链路的可重复诊断方法、当前性能基线和优化判断原则。它不保存原始串口
日志、设备标识或一次性 trace；Scene、compositor 或显示后端发生结构变化后，应重新测量并替换基线。

## 1. 不要只看 CPU 百分比

屏幕上的 CPU 数字是系统非 idle 时间的汇总，不等于 Guest SDK 占用，也不能区分以下工作：

```text
Guest 游戏逻辑和属性更新
  -> SDK 计算净差量并编码 Scene patch
  -> WAMR / Service ABI
  -> Host GuestScene 原子应用和资源验证
  -> AppSurfaceCompositor 规范化、damage 和重放
  -> LVGL Host root 合成
  -> DisplayPipeline shadow copy 和 panel submit
```

前五步发生在 Guest `SceneUpdate::Present()` 的同步路径。LVGL refresh 和最终 panel submit 由 Host 显示
任务异步执行；它们可能通过 LVGL 锁反向增加下一次同步提交的等待时间，但不能直接加到同一帧的
`Present()` 数字里。

## 2. 分段计时方法

测量应使用 release 构建和真机，连续聚合至少 120 帧，分别覆盖稳定移动、粒子/文字变化和 Layer
translation 或全屏特效。计时探针本身应在测量后移除，尤其不要在 Guest 每帧长期调用 Clock Service。

推荐分段：

| 阶段 | 边界 | 能回答的问题 |
|---|---|---|
| Guest world | `BeginUpdate()` 到世界节点更新结束 | App 遍历、属性比较和 setter 是否过重 |
| Guest HUD | HUD 节点更新 | 高频文字和状态 UI 是否过重 |
| Guest present | `Present()` 调用 | SDK 扫描/编码、WAMR、Host 同步提交总成本 |
| Host lock | Service 入口到取得 LVGL 锁 | 是否被异步显示刷新反压 |
| Host apply | `GuestScene::Apply()` | wire 解析、scratch copy 和原子验证成本 |
| Host assets | texture/font 扫描与 retain | 资源引用管理成本 |
| Host normalize | Scene 到固定 draw-operation 数组 | 全 Scene 展开和文字测量成本 |
| Host damage | 新旧 operation 比较和区域合并 | damage 算法成本 |
| Host render | dirty region 恢复和 operation 重放 | PPA/DMA2D/CPU raster 主成本 |
| Display refresh | LVGL refresh start 到 ready | Host root 合成和显示链路时延 |

`Guest present - Host total` 可作为 SDK 净差量扫描、编码和 WAMR/Service 边界的近似值。两端计时器
并不共享起点，所以只应分析 120 帧聚合值，不比较单帧时间戳。

## 3. 当前 Snake 基线

测量日期为 2026-08-30，目标是 ESP-Mosaico/S31 480×480，Snake 以 60 Hz 运行。Scene 中共有 185 个
SpriteBatch instance 容量；稳定移动的 wire 通常只有 2–6 条 record、5–9 个 changed instance 和
268–492 B，damage 通常为 1–2 个小区域。下面是七个无明显显示锁竞争的 120 帧窗口均值：

| 阶段 | 平均耗时 | 观察范围 |
|---|---:|---:|
| Guest world | 1.66 ms | 1.43–1.88 ms |
| Guest HUD | 0.56 ms | 0.49–0.62 ms |
| Guest `Present()` | 8.03 ms | 7.73–8.51 ms |
| Guest Render 总计 | 10.25 ms | 9.65–10.90 ms |
| Host lock | 0.13 ms | 0.07–0.22 ms |
| Host `GuestScene::Apply()` | 0.60 ms | 0.57–0.63 ms |
| Host assets | 0.08 ms | 0.07–0.08 ms |
| Host normalize | 2.12 ms | 2.05–2.20 ms |
| Host damage | 0.39 ms | 0.37–0.41 ms |
| Host render | 2.49 ms | 2.26–2.82 ms |
| Host 同步提交总计 | 6.60 ms | 6.30–6.99 ms |

由此得到当前链路的三个事实：

1. wire 差量已经生效。蛇变长不会让普通帧重新发送整条蛇，patch 解析本身也不是首要瓶颈；
2. Host 每帧展开整个 Scene 的 `normalize` 是最大的固定 CPU 成本之一，即使 wire 只有几百字节仍约
   2.1 ms；dirty region 的实际重放约 2.5 ms；
3. Guest `Present()` 与 Host 同步提交之间约有 1.4 ms，主要包含 SDK 全对象/instance 扫描、编码和
   WAMR/Service 边界。Guest world + HUD 还会为大量最终未变化的对象执行 setter 和 undo snapshot。

发生显示锁竞争的窗口中，Host lock 平均会从约 0.1 ms 增长到 1.9–3.7 ms，Host 同步提交随之增长到
8.6–10.9 ms。稳定 LVGL refresh 常见约 4–10 ms；多区域粒子/特效窗口采样到 23–26 ms。Layer cache
进入/退出或大面积特效时，单帧 Scene damage 约 183k pixel，采样最大 Host render 为 13.5 ms、Host
同步提交为 45.1 ms、Guest `Present()` 为 46.5 ms。最大值用于定位尖峰，不代表稳定帧预算。

硬件计数同时确认 PPA fill/blend 和 DMA2D display shadow copy 正在使用。硬件加速减少的是像素搬运和
混合成本，不会自动消除 Scene 全量遍历、文字测量、ABI 编码或锁等待。

## 4. 当前优化顺序

2026-08-30 第一轮优化已经完成：

1. Guest setter 先比较新旧值再保存事务 undo；`BeginUpdate()` 只清除上次 touched slot；普通 patch 只遍历
   touched nodes/layers/instances，keyframe 保留完整扫描；
2. Snake 不再先隐藏 scenery、snake、flash、combo 和 overlay 的整个固定池，只关闭从 active set 离开的
   slot；
3. Host `GuestScene` 保存最近一次成功 wire 的 property mask；`AppSurfaceCompositor` 只重新规范化变更
   operation，并在双缓冲间同步上帧真正不同的 slot。keyframe、node kind 和 SpriteBatch content 变化自动
   回退完整 normalize；Layer z-order 变化自动重新排序；
4. 调试日志增加 `normalize=<count>/patch|full`，不依赖每帧时钟探针即可持续检查增量路径。

真机复测仍使用同一块 ESP-Mosaico/S31 480×480 和同一 Snake 场景：

| 场景 | 优化前 | 优化后 |
|---|---:|---:|
| 普通移动 CPU / FPS | 约 52% / 60 | 约 38% / 60 |
| Combo + 粒子 CPU / FPS | 约 69% / 52 | 约 43% / 60 |
| 普通帧 normalized operations | 约 207 | 5–10 |

普通帧 wire 仍为约 268–548 B，damage 仍为 1–2 个小区域，没有通过扩大脏区换取 CPU。Layer translation
进入/退出时按设计重新规范化约 207 个 operation，并继续出现 `layer-cache=yes`。顶部下拉状态面板覆盖、
暂停 Guest refresh 和收回均通过真机验收。

下一轮按收益和风险排序：

1. 缩短持有 LVGL 锁的范围，让 App Surface CPU/PPA 合成尽量在锁外执行，只在提交 image damage 和 Host
   root 状态时持锁；
2. `GuestScene::Apply()` 从整 Scene current-to-scratch copy 迁移为 touched-slot undo 或等价的固定容量
   原子事务，但必须继续保证失败零副作用；
3. 缓存 Label metrics，使仅位置、颜色、可见性或祖先 Layer 变化时不重新测量相同文本；
4. 最后再优化 damage 合并阈值和具体 PPA/DMA2D primitive。当前 damage 计算只有约 0.4 ms，优先级低于
   render 和 lock contention。

每次优化都应同时核对 wire record/instance/bytes、damage regions/pixels、operation replay、硬件/CPU
fallback 和 panel submit。只降低某一段计时但扩大 damage 或增加最终 panel 像素，不算有效优化。

## 5. Metalio-Claw4 渲染路径与复制基线

2026-08-30 使用 Metalio-Claw4/P4 720×720 RGB888 产品 profile 复测当时仍使用 opaque board PNG 的
Snake。测试使用 release Host、
正式 Snake Bundle 和屏幕上的 1 秒性能采样；游戏为 Level 1、初始长度 1、无粒子和 shake 的稳定直行。
120 个 Scene 提交窗口从 `#6000` 到 `#6120`，对应约 2 秒、10 个逻辑格。窗口内保持 60 FPS，CPU 约
31%。不保存原始串口日志或设备标识。2026-08-31 起 Snake 的纯色棋盘背景改为 `RoundedRectNode`，下列
数据保留为 opaque texture decode/copy 的历史基线，不再代表当前 Bundle 的 primitive 构成。

稳定窗口端点的普通帧均为 `wire=2rec/5inst/268B`、`normalize=5/patch`、`damage=1/2001 pixels` 和
`replays=6`。`2001 pixels` 是 29×69 的小区域，只占 720×720 屏幕的约 0.39%。三个无特效的
Display refresh 检查点为 6.84 ms、14.59 ms 和 8.48 ms；其中按秒更新的性能浮层也会加入 LVGL damage，
所以这些检查点只用于确认 60 Hz 预算，不替代分段计时窗口。

### 5.1 移动一格产生的 App Surface 操作

Level 1 每 200 ms 移动一格，60 Hz 插值会为一格提交约 12 个 Scene。当时的硬件计数在 120 帧窗口中的
优化前差量如下；`DMA2D copy` 只统计 `AppSurfaceCompositor` 自己的 copy，不包含 LVGL 双 framebuffer
同步：

| App Surface primitive | 120 帧差量 | 平均每帧 | 平均每格 |
|---|---:|---:|---:|
| PPA opaque fill | 535 | 4.46 | 53.5 |
| PPA BGRA blend | 226 | 1.88 | 22.6 |
| PPA scale | 0 | 0 | 0 |
| App Surface DMA2D copy | 0 | 0 | 0 |
| CPU fallback | 240 | 2.00 | 24.0 |
| 合计 | 1,001 | 8.34 | 100.1 |

这个结果可以从 Snake 的 retained Scene 拆成两部分：

1. 仅蛇移动时，每个插值帧先恢复一个 damage 背景，再重放 board、两个 15×15 trail、29×29 head 和
   两个 4×5 eye。12 帧合计为 48 次 PPA fill、12 次 board blend 和 24 次很小的 CPU eye fill，共
   84 次 primitive；
2. food atlas 以 16 帧/600 ms 独立动画，每格期间平均更换约 5.3 次 source frame。每次需要恢复 food
   damage，并重放 board 和 food，平均再增加约 5.3 次 fill 和 10.6 次 blend。两部分相加与真机的
   100.1 次/格一致。

普通移动没有像素几何变形。board 是 625×625 到 625×625，food 是 43×43 到 43×43，snake/trail/eye
本来就是实心矩形；因此没有 scale、rotation 或 resample。位置插值表现为重新 fill 新坐标，不是把旧蛇
位图做 transform。测量时 PNG decoder 把 opaque board PNG 也展开为 BGRA8888，所以 damage 下的 board
恢复走 PPA blend，而不是 BGR888 DMA2D copy。

随后实现 opaque PNG 保留 BGR888。相同的 120 Scene frame 初始 Level 1 窗口中，board 的 PPA blend 从
约 17.2 次/格迁移为 17.4 次/格 App Surface DMA2D copy；剩余约 5.4 次/格 blend 对应透明 food。fill
仍约 54.1 次/格，scale 仍为 0，两个小 eye fill 继续走 CPU。也就是说优化只改变 opaque board 的像素
primitive，没有扩大 damage、改变 Scene 提交数或引入图像变形；board decoded texture 同时由 4 B/px
降为 3 B/px。

### 5.2 App Surface 到 MIPI-DPI panel

Metalio-Claw4 的最终路径与 Mosaico QSPI panel 不同：

```text
Guest Scene patch
  -> AppSurfaceCompositor 在 720×720 RGB888 PSRAM Surface 上重画 damage
  -> LVGL 将 App Surface 的 dirty RGB888 区域画到当前 back framebuffer
  -> LVGL 同步上一 front framebuffer 尚未被本帧覆盖的旧 dirty 残片
  -> DOUBLE_DIRECT 提交完整 framebuffer 指针
  -> MIPI-DPI 在 VSYNC 切换 framebuffer，并持续扫描 RGB888
```

逐层的复制语义如下：

| 边界 | 普通 Level 1 每格 | 实际语义 |
|---|---:|---|
| App Surface 内部（优化前） | 0 次 DMA2D copy、0 次 scale | 小区域由 PPA fill/blend 和两个 CPU eye fill 重建；opaque PNG 优化后的变化见 5.1 |
| App Surface → back framebuffer | 约 17.4 次 CPU image span、90.4 KiB、4.29 ms | opaque RGB888 image 当前走 LVGL CPU fallback；调用数会因一个 refresh 内的多个 dirty region 高于 12 |
| front → back 同步 | 约 11.2 次 DMA2D submit、68.5 KiB、4.06 ms | `refr_sync_areas()` 只复制上帧 dirty 中未被本帧覆盖的残片；多数当前 damage 已覆盖的区域无需同步 |
| back framebuffer → panel | 12 次 pointer submit，0 次整屏 pixel copy | DOUBLE_DIRECT 的 buffer 已经是 DPI framebuffer；含整帧 cache clean 的 driver call 平均约 0.12–0.14 ms/帧 |
| framebuffer cache maintenance | 12 个整屏地址范围 | 每帧仍传入 720×720×3 B，即 1.5552 MB；每格约 18.66 MB、60 Hz 时约 93.31 MB/s 的地址范围，但地址范围不能当作实际耗时 |
| MIPI-DPI scanout | 12 次整屏扫描 | 每格读取约 18.66 MB；这是持续显示带宽，不是软件图像复制 |

以上数字来自关闭性能浮层后的聚合探针，不是由 damage 数量反推。探针分别包围 LVGL RGB888 软件
image fallback、板级 `buf_copy_cb` 的 blocking DMA2D 和 DOUBLE_DIRECT submit。初次分析使用临时探针；
后续已整理为默认关闭的持续 telemetry。
简单直行窗口内，120 次 CPU image span 用时 1.381 秒，共复制 213,007 pixels、耗时 29.645 ms；按
Level 1 每秒 5 格归一化，就是表中的 17.4 次、30,848 pixels 和 4.29 ms/格。另一个无 shake 的 3.205 秒
窗口内，front → back 完成 180 次 DMA2D submit，共 365,896 pixels、耗时 65.114 ms，对应 11.2 次、
22,833 pixels 和 4.06 ms/格。

这也修正了原来的“front → back 约 17 次/格”推算：food 和 snake damage 会增加 App Surface 的软件
image span，但 LVGL 在同步另一 framebuffer 时会扣掉本帧即将重画的交集，所以实际 DMA2D submit 更少。
这些数字仍是典型值而非协议常量；粒子、HUD、damage coalescing 和 shake 都会改变区域数与面积。

### 5.3 DOUBLE_DIRECT 分段耗时

同一批 P4 真机数据中，一个 60 refresh 的纯 Level 1 窗口测得 panel driver call 共 7.063 ms，即
0.118 ms/帧；另一个 180 refresh 无 shake 窗口为 0.134 ms/帧。这个 call 已包含 DPI driver 对完整
1.5552 MB framebuffer 地址范围的 cache writeback 和 pointer submit，因此 cache writeback 自身不会高于
约 0.12–0.14 ms/帧。它目前不是 Snake 的主要瓶颈。

submit 后等待下一次 framebuffer switch 平均为 5.6–5.7 ms/帧。这个数主要表示调用落在当前 VSYNC 周期
中的相位，不是 5.7 ms 的像素复制或可直接消除的 CPU 工作；等待期间 LVGL task 阻塞，panel 仍在扫描。
因此不能把 `display refresh total` 或 VSYNC wait 全部归因于 cache clean。

### 5.4 大面积特效例外和后续优化

shake 使用的是 retained Layer translation 特例。进入时先把 641×641 Game Layer 捕获到 cache，后续帧用
DMA2D copy 移动快照；不做 scale。真机日志在进入/退出时看到约 411k–415k damage、`layer-cache=yes` 和
`scale=0`，因此不能把这个尖峰混入普通移动的每格计数。

分段统计后已完成前两项改进：

1. opaque PNG 在 decode 后保留 BGR888，使当时的 board damage 从 PPA BGRA blend 变为 DMA2D copy，
   同时减少 25% 的 decoded texture 容量；Snake 后续已将这张纯色 board PNG 删除并改用
   `RoundedRectNode`，但该格式策略仍适用于确实包含位图细节的 opaque asset；
2. 将临时探针整理为默认关闭的聚合 telemetry，分开记录 App Surface → FB CPU span、front → back
   DMA2D 和 panel submit/VSYNC wait，不混入 App Surface 的 `hw=.../dma2d:`；
3. dirty-row cache clean 暂不进入实现。完整 cache range 虽然是 93.31 MB/s，但实测 driver call 只有
   0.12–0.14 ms/帧；除非后续 profile 显示它在更复杂场景显著增长，否则不值得先承担多 dirty region、
   PPA/DMA cache ownership、overlay、transition 和 VSYNC 正确性的风险。

App Surface 和 LVGL PPA 硬件选择继续沿用 Mosaico 已有方案：P4/S31 共用一个 `100 pixels` 面积门槛，
裁剪后目标区域 `< 100` pixels 走 CPU、`>= 100` pixels 才允许走硬件；格式、对齐、mask 等其他硬件条件
不满足时仍回退 CPU。copy、fill、blend、scale 不拆分，也不增加板级调度配置。下面的微基准用于解释
固定成本和观察回归，不再作为拆分阈值的实施建议。

持续统计由 `CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY` 控制，默认关闭。打开后 Host 每 120 个
Scene frame 输出一条独立聚合记录；关闭时 RGB888 software-image、framebuffer DMA2D、panel submit 和
VSYNC wait 路径均不调用计时函数，也不更新计数器。

实现后的 Metalio 真机复测确认四组字段都能被采集。一个稳定的 120 Scene frame 窗口记录到 175 次
software-image（318,605 px / 42.036 ms）、78 次 front-to-back DMA2D（118,872 px / 20.722 ms）、
121 次 panel submit（12.957 ms）和 121 次 VSYNC wait（727.958 ms）。这些是诊断窗口而非性能承诺；
统计是 display adapter 全局聚合，系统浮层、特效和其他 LVGL damage 也会计入。

### 5.5 S31 纯软件渲染 A/B

ESP-Mosaico 提供默认关闭的 `CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING` 实验开关。打开后 Host 不注册
PPA 或 DMA2D client：App Surface、纹理缩放和 LVGL draw 走 CPU，display shadow 使用 CPU 行拷贝，
App Hall 与 Status Layer 的硬件转场降级为普通 LVGL/直接切换。CO5300 的 QSPI transport DMA 保持不变，
因此该实验只隔离图形加速器，不改变面板总线配置。

使用独立 build directory 和 sdkconfig，避免污染正常 S31 增量构建：

```sh
export S31_HOST_BUILD_DIR="$PWD/build/host-esp32s31-mosaico-software"
export S31_SDKCONFIG="$S31_HOST_BUILD_DIR/sdkconfig.release"
export S31_SDKCONFIG_DEFAULTS="$PWD/firmware/espressif/sdkconfig.defaults;$PWD/firmware/espressif/sdkconfig.s31.defaults;$PWD/firmware/espressif/sdkconfig.s31-software-rendering.defaults"
bash tools/s31.sh build-host
bash tools/s31.sh flash-host
bash tools/s31.sh monitor --reset
```

启动日志必须同时出现 `PPA/DMA2D disabled`、`App Surface compositor: CPU-only` 和
`displayed shadow copy backend: cpu`。Scene 聚合日志中的 `hw=fill/blend/scale/dma2d` 应始终为零，
`cpu` 计数增长。A/B 时使用同一个 App、关卡、操作序列和日志窗口，比较 `display refresh ... total=`、
Scene 提交间隔、触控响应和画面完整性；转场是否降级属于预期差异，不应混入稳态帧率结论。

S31 不提供标准 RISC-V Vector Extension 编译目标，不能启用 LVGL 的 `LV_DRAW_SW_ASM_RISCV_V`；否则
LVGL 会编译其 RVV C 模拟层而非向量指令。P4 的 LVGL draw buffer 使用 64-byte base alignment 和
48-byte stride alignment；48 是 16-byte CPU/SIMD 对齐与 3-byte RGB888 pixel 的最小公倍数。S31 同样
保留 64-byte base alignment，但全局 stride 必须保持紧凑：当前 ESP LVGL/PPA partial RGB565 bridge
按 packed row 寻址，不读取实际 draw-buffer stride，全局补齐会导致按下态裂纹和系统浮层 snapshot 失败。
App Surface 和它的 layer cache 不经过 LVGL draw-buffer allocator，因此仍单独把 RGB888 storage width
补齐到 16 pixels，并保留逻辑宽度。App Hall cover cache 跟随各板当前的 LVGL RGB888 stride policy，
避免 descriptor stride 与实际缓存布局不一致。

S31 真机在 PSRAM 中以 127×127 像素比较 `LV_DRAW_SW_ASM_NONE` 和 `LV_DRAW_SW_ASM_RISCV_V`。后者在
当前工具链中没有生成 RVV 指令，而是进入 LVGL 的 C 模拟层；每像素耗时如下（越低越好）：

| LVGL software operation | ASM none | RISCV_V 模拟层 |
|---|---:|---:|
| opaque RGB888 fill（对齐 stride） | 10 ns/px | 101 ns/px |
| alpha RGB888 fill | 57 ns/px | 385 ns/px |
| RGB888 image blend | 50 ns/px | 73 ns/px |
| ARGB8888 image blend | 139 ns/px | 927 ns/px |

因此两个产品 profile 都显式保留 `LV_DRAW_SW_ASM_NONE`。同一探针还显示，将 127×3=381-byte 的紧凑
RGB888 stride 补齐到 384 bytes 后，opaque fill 从约 20 ns/px 降为约 10 ns/px；其他混合操作基本持平。

App Surface 的 CPU fallback 现在直接调用 LVGL software blend kernels：纯色填充和等尺寸 blit 不经过
额外中间 buffer；scale 使用 16 行一批、64-byte 对齐的固定 PSRAM scratch 调用 `lv_draw_sw_transform()`，
再写入目标 surface。scratch 在 graphics engine 初始化时一次分配，绘制热路径不分配内存。纹理预缩放也
复用同一路径；只有 scratch 不可用或输入无法由 LVGL 表示时，才保留原 reference compositor 作为安全降级。

### 5.6 S31 LVGL draw buffer 高度 A/B

2026-09-01 在同一台 ESP-Mosaico/S31 上重新比较三种明确配置：40 行双缓冲、480 行单缓冲和 480 行
双缓冲。三组均为 RGB565 PSRAM draw buffer、320 MHz CPU、250 MHz PSRAM、相同的 QSPI/TE/DMA2D
displayed-shadow 路径，并保持 Wi-Fi 和 Remote Control 运行。单双缓冲由临时 sdkconfig defaults 显式选择；
板级实现不再隐式强制双缓冲。测试 workload 每帧完整 invalidate 480×480 屏幕并绘制滚动色条；表中丢弃
首个预热窗口，聚合随后 35–50 秒稳定数据。临时 benchmark 配置和源码在数据确认后已删除。

| Target | buffer 高度 | 存储 | 缓冲数 | draw buffer 总量 | flush/帧 | FPS | render/帧 | refresh/帧 | 完整性异常 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| ESP-Mosaico/S31 | 40 行 | PSRAM | 2（双缓冲） | 76,800 B | 12 | 21.00 | 45.71 ms | 46.16 ms | 0 |
| ESP-Mosaico/S31 | 480 行 | PSRAM | 1（单缓冲） | 460,800 B | 1 | 26.10 | 36.27 ms | 36.71 ms | 0 |
| ESP-Mosaico/S31 | 480 行 | PSRAM | 2（双缓冲） | 921,600 B | 1 | 31.07 | 30.12 ms | 30.56 ms | 0 |

480 行单缓冲相对 40 行双缓冲多使用 384,000 B（375 KiB）PSRAM，FPS 提升约 24.3%，refresh 时延下降
约 20.5%；这部分是减少重复 object traversal 和 flush 次数的收益。保持 480 行再从单缓冲改为双缓冲，
额外使用 460,800 B（450 KiB）PSRAM，FPS 又提升约 19.0%，refresh 时延再下降约 16.7%；这部分是 CPU
render 与 SPI/DMA/displayed-shadow 路径重叠的收益。因此 S31 的全屏双缓冲胜出不是行数混淆造成的，
480 行和双缓冲各自都有可测收益。三组均为 0 面积异常，没有 panic 或 reset；S31 保留 480 行 PSRAM
双缓冲产品默认值。这里的 FPS 是最坏方向的整屏 invalidate 吞吐，不代表 App Hall 或典型局部 damage
刷新帧率。

### 5.7 ESP32-S3-BOX-3 LVGL draw buffer 高度与缓冲数 A/B

同日使用同一台 ESP32-S3-BOX-3 比较 40 行双缓冲、80 行双缓冲、240 行单缓冲和 240 行双缓冲。四组均为
RGB565 PSRAM draw buffer、240 MHz CPU、80 MHz PSRAM、40 MHz SPI DMA，并运行与 S31 同构的 320×240
全屏滚动色条 workload。每组的生成 sdkconfig、启动日志和屏幕 capture 都同时标明高度与单双缓冲；表中
聚合 45–60 秒稳定窗口。

| Target | buffer 高度 | 存储 | 缓冲数 | draw buffer 总量 | flush/帧 | FPS | render/帧 | refresh/帧 | 完整性异常 |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| ESP32-S3-BOX-3 | 40 行 | PSRAM | 2（双缓冲） | 51,200 B | 6 | 22.17 | 43.12 ms | 43.53 ms | 0 |
| ESP32-S3-BOX-3 | 80 行 | PSRAM | 2（双缓冲） | 102,400 B | 3 | 22.55 | 42.46 ms | 42.87 ms | 0 |
| ESP32-S3-BOX-3 | 240 行 | PSRAM | 1（单缓冲） | 153,600 B | 1 | 19.78 | 48.67 ms | 49.09 ms | 0 |
| ESP32-S3-BOX-3 | 240 行 | PSRAM | 2（双缓冲） | 307,200 B | 1 | 23.41 | 40.83 ms | 41.24 ms | 0 |

S3 没有 S31 的 PPA/DMA2D displayed-shadow 路径。240 行单缓冲虽然把 flush 从每帧 6 次降为 1 次，
但更大的 PSRAM 连续绘制工作集令 FPS 相对 40 行双缓冲下降约 10.8%，refresh 时延增加约 12.8%。同为
240 行时，双缓冲相对单缓冲提升约 18.3% FPS，说明 CPU render 与 SPI DMA 的重叠对全屏 buffer 至关
重要。最终 240 行双缓冲比 40 行双缓冲快约 5.6%，refresh 时延低约 5.3%，代价是多用 256,000 B
（250 KiB）PSRAM。四组均为 0 面积异常；在没有典型产品 workload 数据证明这 5.6% 的最坏方向吞吐
80 行双缓冲相对 40 行双缓冲多使用 51,200 B（50 KiB）PSRAM，但 FPS 只提升约 1.7%，refresh 时延只
下降约 1.5%，不足以证明常驻增加这部分内存值得。240 行双缓冲虽再快约 3.7%，却还要多使用 200 KiB。
因此 S3 preview 保留 40 行 PSRAM 双缓冲作为默认；80/240 行组合只保留测试数据，不再保留可构建的
benchmark profile。

## 6. PPA/DMA2D 面积微基准

2026-08-30 在 ESP-Mosaico/S31 上使用与 App Surface 相同的 PSRAM、RGB888/BGRA8888 格式和 blocking
driver API 进行一次性微基准。每组先预热四次，再按面积执行 6–300 次；表中为单次平均耗时。`PPA 2x`
的输入边长是表中尺寸的一半，输出边长是表中尺寸。探针在测量后移除，不属于产品启动路径。

| 输出尺寸 | 像素 | DMA2D copy | PPA blend | PPA 2x | PPA fill |
|---|---:|---:|---:|---:|---:|
| 4×4 | 16 | 231 us | 315 us | 138 us | 37 us |
| 16×16 | 256 | 233 us | 333 us | 144 us | 47 us |
| 24×24 | 576 | 259 us | 346 us | 146 us | 48 us |
| 32×32 | 1,024 | 269 us | 360 us | 158 us | 51 us |
| 64×64 | 4,096 | 347 us | 503 us | 214 us | 79 us |
| 128×128 | 16,384 | 581 us | 883 us | 468 us | 209 us |
| 256×256 | 65,536 | 1,488 us | 2,562 us | 1,382 us | 656 us |
| 384×384 | 147,456 | 3,036 us | 5,333 us | 2,937 us | 1,435 us |

结果不是“无论面积都一样”，而是明显的固定提交/同步/Cache 成本加上随面积增长的像素成本。小尺寸区间
主要由固定成本主导：DMA2D copy 约 230 us、PPA blend 约 315 us、PPA 2× 约 138 us、PPA fill 约
40 us。大尺寸区间则近似随输出面积增长。

同一探针也测量了当前 CPU reference compositor。硬件开始胜出的交叉区间为：

| 操作 | CPU 更快的采样点 | 硬件更快的采样点 | 估算交叉面积 |
|---|---:|---:|---:|
| DMA2D RGB888 copy | 16×16 | 24×24 | 约 460 px |
| PPA BGRA blend | 24×24 | 32×32 | 约 840 px |
| PPA 2× scale | 16×16 | 24×24 | 约 270 px |
| PPA opaque fill | 16×16 | 24×24 | 约 420 px |

这些交叉区间说明统一门槛会让部分小图元承担额外的固定成本，但当前产品选择保持 Mosaico 与 P4 的共享
实现和单一 `100 pixels` 面积门槛，不按 SoC 或 operation 分叉。后续只有端到端 telemetry 显示该成本
成为主要瓶颈时，才重新评估调度策略；硬件选择始终不暴露给 Guest 或 Sprite API。
