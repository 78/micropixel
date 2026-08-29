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

## 5. PPA/DMA2D 面积微基准

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

因此一个统一的 `100 px` 硬件门槛只适合做保守 bring-up，不适合作为最终调度策略。候选 S31 门槛可从
copy/fill 512 px、blend 1,024 px、scale 320 px 开始，再用真实 Snake/Blocks 帧做端到端 A/B。P4 需要独立
复测，不能直接继承 S31 的交叉点；最终配置应按 SoC 和 operation 分开，而不是由 Guest 或 Sprite API
暴露硬件选择。
