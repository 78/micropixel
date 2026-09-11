# Gravity Balls · 重力彩球

一个独立的 Guest 物理演示 App：24 个彩色三维球体装在一个固定在设备上的透明盒子里，加速度计
（有则加上陀螺仪）提供盒内的惯性力，倾斜、甩动、旋转设备都会让球群像真实盒子里的弹珠一样运动。
纯黑背景、线框盒体、逐球光晕和远墙光斑面向 AMOLED 的黑色设计。不依赖其他 App、外部物理库或图片素材。

## 操作

- 倾斜、甩动、旋转设备：球群随惯性力运动。盒子的开口就是屏幕玻璃，球会撞在玻璃、侧壁和远墙上。
- 点击画面：给所有球施加确定的弹起速度，观察三维碰撞和前后遮挡。
- 点击顶部 1/8 区域：重置球群。
- 右上角 `MENU`：打开设置面板，`-`/`+` 调节球数（8/16/24/32/48/64，新增的球从玻璃侧的初始格位落下，
  已有的球继续运动）、容器深度（6/9/12/15 单位，线框实时重建）和重力倍数（×0.5/×1/×1.5/×2，乘在
  传感器加速度上）。面板打开时模拟继续运行，点面板外关闭。触摸事件是逻辑显示坐标（Mosaico 为 720×720），
  App 按 `buffer / RendererInfo::width()` 换算到 480×480 buffer 像素后再做命中判断。
  `micropixel --transport usb input tap X Y` 能到达 Direct Surface 中的 App，坐标是面板物理像素
  （Mosaico 0..479、Claw4 0..719），Host 再换算成逻辑坐标；越界会返回 `sequence_failed`。
- 没有加速度计或超过 500 ms 未收到新样本：重力缓慢回到指向远墙的 9.8 m/s²，仍可触摸弹起。
- 系统菜单与返回大厅沿用 Host 手势。

## 物理

盒子坐标系固定在设备上。加速度计读数的相反数就是盒内的“重力 + 墙壁推力”：静止时指向下方，甩动时
自动包含墙壁推球的惯性力，因此不需要姿态融合。陀螺仪可选：有则额外加入旋转参考系的离心力
`-ω×(ω×r)`、欧拉力 `-dω/dt×r` 和科里奥利力 `-2ω×v`，没有则只用加速度计，效果仍然成立。

传感器轴沿用 Tilt 约定：屏幕向右为 `-X`、向下为 `+Y`，世界 Y 向上，所以重力为 `(-ax, -ay, +az)`。
`kDepthAxisSign` 决定加速度计 z 轴对应“朝屏幕里”的符号，按标准 MEMS 约定（朝上的轴静止读 +1 g）取 `+1`；
如果平放时球贴在玻璃而不是落到远墙，翻转该常量。陀螺仪按同一传感器坐标系映射为 `(wx, wy, -wz)`，
离心项与符号无关，欧拉、科里奥利项的方向需要真机确认。

世界单位不是米：`World::kMetersToUnits = 16`，8.8 单位宽的盒子相当于 55 cm 的弹珠箱，传感器加速度
（m/s²）乘以该系数后才作为重力，否则 4 单位的落差要 0.9 s，看起来像在水里漂。固定 240 Hz 半隐式 Euler
积分，每帧最多推进 8 步，多出的时间丢弃而不是追赶，避免慢帧滚雪球。默认 24 个（最多 64 个）球使用不同半径、相同密度
（质量正比于半径立方），进行全部球对的三维碰撞检测。每步做 2 次接触求解，包含法向恢复系数、切向摩擦冲量、
穿透修正、盒壁碰撞、60 单位/秒速度上限和每步 0.05% 的微弱空气阻力。低于 `kRestSpeed` 的接触关闭弹性，
降低静止抖动。加速度计输入限幅 40 m/s²、单样本低通 0.6，甩动响应保持锐利。

## 渲染

相机位于原点看向 +z，盒子前表面在深度 10 处正好填满 buffer，默认深度 12（可在菜单选 6/9/12/15）；宽高按 buffer 比例放缩，
大于 480 像素的面板以一半物理分辩率渲染（Claw4 720×720 → 360 buffer，Host 2× 放大）。720 原生渲染在
Claw4 上实测过：球贴近玻璃时 sprite_px 约 20 万、update 33–36 ms，只有 17–23 fps，所以保留放大。每帧的绘制记录：

- 脏矩形清除：背景纯黑，因此只对上一帧在**同一个 buffer** 上画过的矩形 `FillRect` 黑色（3 个 buffer 各自记录），
  球和它下方的光斑相交时合并为一个矩形；矩形总面积超过 60% 屏幕时改为一次整屏清除；暂停恢复后重新整屏清一次。
- 线框盒体：远墙矩形、四条深度棱、两道深度标尺和远墙网格共 22 条 2 px 宽 `FlatQuad`，亮度按深度衰减
  （调色板 0..7 行的 253/254 号条目）。
- 每个球一条 `AdditiveSprite` 远墙光斑（球在远墙上的投影，越高越大越淡，贴墙时被球盖住而省略）、一条
  `AdditiveSprite` 光晕（1.6 倍直径、峰值 30% 颜色）和一条不透明 `Sprite` 球体。先画全部光斑和光晕，再按远到近画
  球体：光晕只照亮背景，被球体遮住，不会叠加到相邻球的高光上把球堆变成白块。球体来自启动时烘焙的 INDEX8
  图集：11 个直径（480 buffer 下 14–52 px，更大的 buffer 按比例放大到最多 1.5×，避免近处球被放大出锯齿）的预光照球面，高光只向白色混合 55%，边缘覆盖率乘进亮度，在黑底上等价于抗锯齿；
  调色板 0..7 行选颜色，8..15 行为光晕、16..23 行为光斑。
- 标题一条 `Text`（fps 等状态交给 Host 的 FPS 显示，不再画状态行），`MENU` 按钮一条 `FillRect` 加一条 `Text`；
  标题放在 `RendererInfo::safe_area()` 内（圆屏的角部 inset，逻辑坐标换算成 buffer 像素），MENU 贴 buffer 右上角；设置面板打开时再加约 30 条
  半透明 `FillRect`/`Text`，面板矩形记入脏矩形。

`AdditiveSprite` 依赖 Host 的 `MICROPIXEL_GRAPHICS_CAP_RASTER_SPRITE_ADDITIVE`（Sprite `ADDITIVE` 标志，
按 R/G/B 通道饱和相加）；旧 Host 上退回不画光晕和光斑。没有 `polygon_supported()` 时不画线框。

## 遥测

启动时输出一行配置：buffer 尺寸与放大倍数、是否直接扫描输出、面板最高帧率、polygons/additive 能力、
加速度计/陀螺仪是否打开。之后每 120 帧输出一行：

```
gravity-balls: frames=120 fps_x100=… wait_us=… physics_us=… geometry_us=… update_us=… present_us=…
    records=… clear_px=… sprite_px=…
gravity-balls: sim balls=… depth_x10=… gravity_x10=… steps_x100=… dropped=… slow=… z_avg_x100=… speed_avg_x100=… imu_hz_x10=… gyro_hz_x10=…
```

- `wait` 是等待空闲 buffer 的时间（面板受限时增大）；`physics` 含传感器读取和积分；`geometry` 是投影与排序；
  `update` 是 `HostSurface::Update` 的同步时间，包含 Guest 编码和 Host kernel；`present` 是提交调用。
- `records` 是每帧记录数，`clear_px` 是脏矩形清除像素数（整屏为 230400），`sprite_px` 是三类 sprite 的目标像素数。
- `balls` / `depth_x10` / `gravity_x10` 是当前设置。
- `steps_x100` 是每帧物理步数 ×100，`dropped` 是因每帧上限丢弃的步数，`slow` 是超过 50 ms 的帧。
- `z_avg_x100` / `speed_avg_x100` 是球心平均深度（相对盒中心，远墙为 +深度/2）和平均速度，用来在没有画面时
  判断球是否落到远墙、是否在运动。
- `imu_hz_x10` / `gyro_hz_x10` 是本窗口实际收到的新样本率（每帧只读最新样本，因此不超过帧率）。

屏上不再显示 fps，用 Host 的 FPS 显示或上面的遥测行。

## 构建与验证

在仓库根目录、已激活项目工具链的环境中：

```sh
python3 tools/micropixel package guest/apps/gravity-balls --aot-target riscv32-ilp32f
bash tools/tests/test_firmware_host.sh
```

物理回归覆盖正面碰撞的动量与能量、完全重合球心、150 秒反复弹起和变化重力下的有限值与盒壁边界、
20 秒静止收敛，以及离心和欧拉项的方向。测试通过仓库 Host test 入口执行，测试源码不进入 Bundle。

安装运行使用标准单 App 安装流程；Host 需要包含 `CAP_RASTER_SPRITE_ADDITIVE` 的版本才有光晕与光斑：

```sh
python3 tools/micropixel --transport usb run guest/apps/gravity-balls
```

真机验收：平放后球落到远墙（否则翻转 `kDepthAxisSign`）、四向倾斜方向正确、甩动时球撞玻璃与侧壁、
旋转时离心方向正确、暂停恢复无大时间步和残影、横竖屏文字可读且球群完整可见；对照遥测确认
`clear_px` 远小于整屏、`dropped` 接近 0，并记录实际 fps。没有画面时可用遥测里的
`z_avg_x100` / `speed_avg_x100` 判断运动状态。

2026-09-11 Mosaico（S31）实测（`--profile performance`，面板上限 42 fps）：静止 32 fps 的首版经过
缩小光晕/光斑、合并脏矩形、光晕先于球体绘制后，静止与甩动时均为 35–39 fps（update 19–21 ms、physics 3.5–3.8 ms、
clear_px 70k–100k、sprite_px 77k–92k、records 116–125、dropped 每窗口 ≤ 30 步，已出现 1–2 ms 的 `wait_us`，
说明开始受面板刷新率限制）。剩余瓶颈是 Sprite 内核对 PSRAM 的读改写（加法叠加约 110–130 ns/px）；
2026-09-11 之前的原型（每球 168 三角形、整屏清除）同一设备约 9 FPS。
