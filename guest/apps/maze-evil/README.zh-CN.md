# Maze Evil

Maze Evil 是一个本地 ESP-IDF 原生 raycaster demo（不在仓库内）的 Guest 移植版，也是 Graphics 1.5 Direct Surface 和
Graphics 1.6 Host 光栅 kernel 的验收载体。Guest 只做几何，所有像素由 Host 光栅 kernel 写进 Host 持有的
`HostSurface` 双缓冲并直接扫描输出；Guest 从不映射帧缓冲，Bundle 不需要 `pinned_memory`。
关卡与 AI 以 demo 为基础。场景、怪物、道具和特效使用 ImageGen 原画离线量化为现有 256 色索引素材，
编入 Guest 的只读数据；枪采用右手持枪布局。运行时只构建光照调色板，不解码 PNG 或生成材质噪声。
Bundle 的资源包仍只携带 Opus BGM 和启动图标。

中英文系统的显示名统一为 `Maze Evil`；游戏内像素字形标题为 `MAZE EVIL`。
工程目录为 `guest/apps/maze-evil/`，App ID 为 `micropixel.maze-break`，性能日志前缀为 `maze-break-bench:`。
新 ID 会作为独立应用安装；旧 ID 的安装和数据不会自动迁移。

- 渲染：`game/renderer.*` 按 `gfx::ViewConfig` 的运行时宽高工作，同一 Bundle 可跑 480×480（Mosaico）、
  720×720（Claw4）和 S3 的合成回退路径。启动时 `UploadResources()` 把 9 张 128×128 场景纹理、
  16 张精灵（补到 2 的幂）、一张 128×32 字形图集和 16 级 lit 调色板（canonical RGB565，字节序由 Host 处理）
  上传给 Host，共 26 个 slot。墙纹理已离线排为列主序，地板/顶棚为行主序。每帧只做光线投射、地板行设置、
  精灵排序与深度测试，往 `RasterDrawList` 追加记录：`SpanPair` 画地板/天花板，`Column` 画墙、门和世界精灵，
  `Sprite` 画武器与枪口
  火光，`SolidSprite` 画 HUD 字形，`FillRect` 画状态栏、十字线、虚拟摇杆和伤害/死亡蒙层。先投射全部墙柱，
  再只对未被墙遮住的 x 段画地板/天花板（16 列一块粗筛），少写约 40% 像素。绿通道与红蓝一样量化到 5 bit，
  避免 RGB888 面板补零展开时灰阶偏绿。
- 开始页：以静止的游戏第一帧为背景，用左右区域、四向摇杆、转向箭头和右侧中部开火圆圈说明操作。
  全屏点击或按下并松开任意 Guest 按键后开始世界和 BGM；死亡或通关后同样可全屏点击或按键返回说明页，
  无需命中开火热区。每次页面切换需要新的按下、松开，取消的触摸或按键不触发确认。
  `--benchmark` 自动跳过说明。
- 输入：默认纯触摸。左半屏浮动摇杆控制前进、后退和左右平移；右侧拖动控制水平转向，右下方留给拇指滑动，另一根手指可同时按住右侧中部开火键。
  右侧中部常驻 `FIRE` 圆圈，按下即开火、按住连发，松开停止；确认键也可开火。
  圆圈半径为屏幕短边的 1/10，触摸热区半径为短边的 1/8（720 px 屏幕分别为 72 / 90 px），
  按下时优先判定开火热区。每个触点在按下时确定职责，滑入其他区域不会切换职责或误开枪；
  触点取消和恢复应用时清理按住状态。多指同时移动、转向、开火取决于面板支持的触点数量。
  `--motion` 可显式开启体感辅助移动和瞄准，长按确认键 1.5 s 重新归中；`--no-motion` 强制关闭。
  体感在后台归中，3 秒未完成会退回纯触摸。
- 纪录：开始页显示 `BEST`，游玩和结算页显示 `TIME / BEST`，统一使用 `00:00`（分:秒），不显示小数秒。只累计前台实际游玩时间，
  不包含说明页、暂停和结算等待，也不使用限幅后的模拟时间。通关结算只显示本次 `TIME` 和本轮开始前的 `PREV BEST`（首次为 `--:--`）。严格快于旧成绩时更新纪录，
  用应用 KV 存储的 `level1_v1_ms` 保存毫秒成绩；平局、死亡和 benchmark 不更新纪录。
  保存失败会在结算页提示，当前会话仍保留成绩。修改地图或通关规则时需更换纪录键。
- 关卡：当前仅一张固定地图，直接定义在 `game/level.cpp` 的字符数组中，并非 JSON 或随机生成。
  `World::Reset()` 将字符转换为墙、门、敌人和道具；击杀全部敌人并贴近出口后通关。
- 音频：16 个音效只写在 [`audio/sfx.json`](audio/sfx.json)，BGM 用 `assets/bgm_loop.ogg` 循环播放；
  `--no-bgm` 关闭 BGM 以便测量。
- 数学：Guest 不链接 libm，`rc_math.hpp` 用 Wasm 指令和短多项式提供 `sin/cos/atan/sqrt/floor`。

启动参数：

| 参数 | 作用 |
|---|---|
| `--benchmark` | 固定 1/40 s 步长、固定 RNG 种子和脚本化自动漫游，每 120 帧输出 `maze-break-bench:` 一行；默认静音，`--sound` 恢复 |
| `--perf` | 正常游玩时同样输出统计并显示 HUD 的 FPS / RENDER / PRESENT / WAIT |
| `--mute` / `--no-bgm` | 关闭全部声音 / 只关 BGM |
| `--motion` / `--no-motion` | 开启体感辅助 / 强制纯触摸（默认） |

统计行字段与 demo 日志对齐：`render_avg_us`（几何 + 记录编码 + Host kernel 执行）、`present_avg_us`
（`SURFACE_PRESENT` 调用）、`wait_avg_us`（等待 `SURFACE_RELEASED` 归还 buffer）以及 `frame_max_us`。

应用使用 Host buffer 与 Host Raster 内核。面板宽于 480 px 时自动使用 `upscale = 2`，
减少填充像素与内存带宽需求；触摸摇杆坐标按 `upscale` 换算。性能 HUD 会改变绘制工作量，
对比测量时应固定其状态。方法见 [图形性能诊断](../../../docs/development/graphics-performance.zh-CN.md)。

示例直接使用 `gfx/textures.cpp`、`gfx/sprites.cpp` 中的索引像素数据，以及 `assets/launch.png` 和
`assets/bgm_loop.ogg`，正常构建无需原始素材或美术生成工具。

`assets/source/` 是本地美术工作目录，包含原画、提示词和生成脚本，由 Git 忽略，不随 GitHub 示例发布。
如需重新制作素材，在保留此目录的本地环境中修改原画或生成器并导出，再更新示例使用的素材；不手改像素数组。

- 怪物动作帧为 64×64；受伤帧从行走 A 提亮并改变眼睛颜色，倒地精灵保持同一画布并贴底，避免尸体悬空。
- 火把为 32×64 四帧，8 fps 循环；第 21 行以下固定复用 A 帧，只有火焰变化，四帧均保持自发光。
- 枪为 128×96，放在右下方，以 1× HUD 比例绘制；56×40 枪口火光与枪口位置同步走动和后坐。
- 七种可平铺纹理只在四边 4 texel 范围内做接缝混合，转换后对边索引完全一致；门和出口保留独立边框。
- 图片中的透明度离线转成 index 0，其他像素不使用透明键。Host 仍使用已有索引光栅 kernel；最大上传
  scratch 仍为 128×128，新增美术不增加每帧解码、动态分配或绘制 pass。

构建、部署与基准：

```sh
python3 tools/micropixel package guest/apps/maze-evil --aot-target riscv32-ilp32f
python3 tools/micropixel --transport usb run guest/apps/maze-evil --no-follow -- --benchmark
python3 tools/micropixel --transport usb logs -n 40
```

## 商店资料

[玩法说明](store/description.md)、[版本说明](store/release-notes.txt)，以及
[开始页](store/01-start.jpg)和[游戏画面](store/02-playing.jpg)用于商店发布。
