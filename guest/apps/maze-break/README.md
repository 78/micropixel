# 迷城突围 / Maze Break

迷城突围（Maze Break）是一个本地 ESP-IDF 原生 raycaster demo（不在仓库内）的 Guest 移植版，也是 Graphics 1.5 Direct Surface 和
Graphics 1.6 Host 光栅 kernel 的验收载体。Guest 只做几何，所有像素由 Host 光栅 kernel 写进 Host 持有的
`DirectSurface` 双缓冲并直接扫描输出；Guest 从不映射帧缓冲，Bundle 不需要 `pinned_memory`。
关卡、AI、纹理和精灵与 demo 逐字对应，全部在运行时按程序生成，Bundle 只携带 Opus BGM 和启动图标。

显示名按系统语言选择 `迷城突围`（`zh-CN`）或 `Maze Break`（默认英语）；游戏内像素字形标题为 `MAZE BREAK`。
工程目录为 `guest/apps/maze-break/`，App ID 为 `micropixel.maze-break`，性能日志前缀为 `maze-break-bench:`。
新 ID 会作为独立应用安装；旧 ID 的安装和数据不会自动迁移。

- 渲染：`game/renderer.*` 按 `gfx::ViewConfig` 的运行时宽高工作，同一 Bundle 可跑 480×480（Mosaico）、
  720×720（Claw4）和 S3 的合成回退路径。启动时 `UploadResources()` 把 9 张 128×128 墙/地板纹理（`gfx/textures.cpp` 在 64×64
  设计网格上生成再按 `kTextureSize/64` 拉伸，砖块、面板等结构不变，只让颗粒更细；列主序/行主序）、
  14 张精灵（转成列主序并补到 2 的幂）、一张 128×32 字形图集和 16 级 lit 调色板（canonical RGB565，字节序
  由 Host 处理）上传给 Host，共 24 个 slot。每帧只做光线投射、地板行设置、精灵排序与深度测试，往
  `RasterDrawList` 追加记录：`SpanPair` 画地板/天花板，`Column` 画墙、门和世界精灵，`Sprite` 画武器与枪口
  火光，`SolidSprite` 画 HUD 字形，`FillRect` 画状态栏、十字线、虚拟摇杆和伤害/死亡蒙层。先投射全部墙柱，
  再只对未被墙遮住的 x 段画地板/天花板（16 列一块粗筛），少写约 40% 像素。绿通道与红蓝一样量化到 5 bit，
  避免 RGB888 面板补零展开时灰阶偏绿。
- 开始页：以静止的游戏第一帧为背景，叠加人物面对竖立设备的侧视图说明握持姿势，用覆盖实际左右半屏的半透明双色区域、
  四向摇杆与准星图标说明左侧移动和右侧开火，并辅以简短体感瞄准提示；松开任意触点或确认键后开始校准，
  保持屏幕舒适地竖立在面前并保持稳定。校准完成才启动世界和 BGM；3 秒未完成则退回触摸说明页，
  再次点击开始。死亡或通关后点击重玩也回到这一静止引导页，必须重新按下并松开触点或确认键
  才开始校准，重玩那次触摸的松开不会跳过说明。`--benchmark` 自动跳过说明与校准等待。
- 输入：默认使用 Sensors `Accelerometer/Gyroscope` 的姿态无关体感（前后倾斜前进/后退，左右倾斜转向，
  挥动瞄准），右半屏点击开火、长按 Function Button 1.5 s 重新校准中立姿态；`--no-motion` 退回纯触摸
  （左半屏虚拟摇杆，右半屏视角/开火）。
- 音频：16 个音效只写在 [`audio/sfx.json`](audio/sfx.json)，BGM 用 `assets/bgm_loop.ogg` 循环播放；
  `--no-bgm` 关闭 BGM 以便测量。
- 数学：Guest 不链接 libm，`rc_math.hpp` 用 Wasm 指令和短多项式提供 `sin/cos/atan/sqrt/floor`。

启动参数：

| 参数 | 作用 |
|---|---|
| `--benchmark` | 固定 1/40 s 步长、固定 RNG 种子和脚本化自动漫游，每 120 帧输出 `maze-break-bench:` 一行；默认静音，`--sound` 恢复 |
| `--perf` | 正常游玩时同样输出统计并显示 HUD 的 FPS / RENDER / PRESENT / WAIT |
| `--mute` / `--no-bgm` / `--no-motion` | 关闭全部声音 / 只关 BGM / 关闭体感 |

统计行字段与 demo 日志对齐：`render_avg_us`（几何 + 记录编码 + Host kernel 执行）、`present_avg_us`
（`SURFACE_PRESENT` 调用）、`wait_avg_us`（等待 `SURFACE_RELEASED` 归还 buffer）以及 `frame_max_us`。

Mosaico（ESP32-S31）实测（`--benchmark`，480×480，performance profile，Direct Surface 独占扫描，
Host 性能 HUD 开启）：

| 像素路径 | render avg | Host HUD fps |
|---|---|---|
| Host buffer + Host 光栅 kernel（当前） | ~23 ms | 40–41（面板上限 42） |
| 旧 Guest buffer + Guest 逐像素（已删除，参考） | ~29–32 ms | 30–33 |
| 原生 ESP-IDF demo（参考） | 17–23 ms | 40+ |

Guest 逐像素路径已删除：Host kernel 是唯一一条到 40 fps 的路径（历史数据见
`docs/development/graphics-performance.zh-CN.md` §10.1）。Metalio-Claw4（ESP32-P4，720×720）全分辩率时 Host
buffer + kernel 约 59 ms、16–17 fps，瓶颈是 PSRAM 写带宽（1 MiB 目标即使 memset 也要 17.5 ms）；因此面板宽于
480 px 时 App 自动用 `upscale = 2`（360×360 Host buffer，PPA 放大到 720×720），`render` 约 20 ms、39–40 fps，
触摸摇杆坐标按 `upscale` 换算。系统菜单里开启性能 HUD 后，Direct Surface 独占期间它由 Host 直接 blend 进在飞的帧、
传输后恢复，整帧仍是一笔零拷贝传输，只多约 0.7 ms。

重新生成确定性素材：

```sh
python3 guest/apps/maze-break/assets/source/generate_bgm.py
python3 guest/apps/maze-break/assets/source/generate_launch_icon.py
```

构建、部署与基准：

```sh
python3 tools/micropixel package guest/apps/maze-break --aot-target riscv32-ilp32f
python3 tools/micropixel --transport usb run guest/apps/maze-break --no-follow -- --benchmark
python3 tools/micropixel --transport usb logs -n 40
```
