# Juicy Blocks

Juicy Blocks 是与 Juicy Snake 同系列的 720×720 触控俄罗斯方块 Guest App。界面沿用近黑终端 HUD、
关卡强调色、`TextButton` 和固定容量运行时结构。棋盘使用静态 atlas 和固定容量 SpriteBatch，
活动块移动只提交变化格子的属性，不再上传像素。

```text
blocks/
├── app.json
├── main.cpp
├── blocks.hpp
├── blocks_common.hpp
├── blocks_model.hpp/.cpp       # 10×20 棋盘、7-bag、Hold、Ghost 和计分
├── blocks_game.hpp/.cpp        # 状态机、触控、计时和存档
├── blocks_renderer.cpp         # HUD、棋盘 Batch 与格子属性差量同步
├── blocks_audio.cpp            # 由统一 profile 驱动的短音效
├── audio/                      # 音效参数、感知约束和校准说明
├── blocks_app.cpp              # 资源加载与事件循环
├── blocks_model_test.cpp       # 可原生运行的规则回归
└── assets/
    ├── manifest.json
    └── source/                 # 启动图、棋盘素材及生成器/测试
```

构建 App Bundle：

```sh
python3 tools/micropixel package guest/apps/blocks --aot-target riscv32-ilp32f
```

触控操作覆盖整个 720×720 逻辑屏幕：任意位置点击旋转、水平拖动、慢速下拖软降、快速下划硬降、上划换块；
轻点 HOLD 换块，轻点左上角标题所在的顶部 HUD 区域暂停。暂停热区覆盖完整标题，但不侵入棋盘；从 HOLD 或标题区域起手的滑动仍按游戏手势处理，不会被按钮截断。
按住并向下拖动时，方块只按手指位移逐格软降，不会在 Move 阶段提前硬降。只有松手时，手势同时满足
150 ms 内完成、向下至少移动 80 个逻辑像素且纵向位移占优，才执行硬降。

游戏每消除 10 行提升一级，最高为 99 级。自动下落周期在四个速度点之间线性变化：
1 级 750 ms、12 级 240 ms、20 级 200 ms 和 99 级 100 ms。到达 99 级后等级和下落周期都不再变化。
内部使用微秒精度，软降和硬降仍允许熟练玩家主动加快节奏。

棋盘使用静态背景 Sprite 和固定容量 200 的 `SpriteBatch`，每个格子对应一个稳定槽位。
Guest 保留 visual-cell code 缓存，活动块、Ghost、落定方块和消行闪烁只更新变化格子的 atlas source
或可见性；`SyncPlayfield()` 提交一个 Scene 事务，完整 `Render()` 把棋盘与 HUD 合并在同一事务中。
不再创建 StreamingTexture、逐格生成像素或上传像素缓冲。

静态 atlas 保留原来的圆角轮廓、顶部高光、22×22 空心 Ghost，以及五套主题各八级消行闪烁。
Hold/Next 共用 atlas 的原尺寸预览图案（含暗色 Hold），固定使用八个实例。背景保留圆角边框，
按显示缩放选择一或两个逻辑像素宽的网格。右侧卡片继续使用 `RoundedRectNode`，按钮使用 `TextButton`。
纹理 Batch 不支持颜色 tint，因此消行颜色也烘焙在 atlas 内，通过 source 切换。

素材生成器从 `blocks_common.hpp` 读取方块和主题配色；修改配色或像素几何后重新生成源 PNG：

```sh
python3 guest/apps/blocks/assets/source/generate_playfield.py
python3 guest/apps/blocks/assets/source/test_playfield.py
python3 tools/micropixel package guest/apps/blocks --aot-target riscv32-ilp32f
```

生成与素材测试需要 Pillow。生成的源 PNG 随应用维护，资源 pack 和 Bundle 仍只由正式构建产生。

顶部 HUD 与 Juicy Snake 使用同一套圆角屏布局规则：标题和右侧 Level/Score/Best、Combo 分别消费
`RendererInfo::safe_area_insets()` 的左右内缩，标题另保留 12 个逻辑像素的视觉 padding。

音效参数只维护在 `audio/sfx.json`。`tools/analyze_sfx.py` 逐采样复现 Host 合成器，结合可替换的设备
频响计算 A-weighted 事件能量、重复暴露、尖锐度代理、瞬态和层级评分；构建会生成报告及 Guest 头文件，
并在感知约束回归时失败。算法、WAV 导出和扬声器校准方法见 `audio/README.md`。

可通过统一 CLI 从 USB Serial/JTAG 注入同路径触控并立即抓图：

```sh
python3 tools/micropixel --transport usb --port /dev/cu.usbmodem1101 input tap 360 352
python3 tools/micropixel --transport usb --port /dev/cu.usbmodem1101 input tap 560 230
python3 tools/micropixel --transport usb --port /dev/cu.usbmodem1101 \
  input swipe 520 300 610 300 --duration-ms 120 --steps 3
python3 tools/micropixel --transport usb --port /dev/cu.usbmodem1101 \
  input swipe 560 250 560 520 --duration-ms 100 --steps 2 \
  --screenshot build/captures/blocks-playing.jpg
```

规则回归可原生运行。真机集成需检查菜单、暂停/继续、满盘、旋转、硬降、Hold/Next、Ghost
与单行/多行消除，确认没有残影，圆角和高光在 720 与 480 物理分辨率下可见。
启动日志应显示 `retained playfield batch with static rounded-block atlas`。
性能验收同时比较 Guest 提交、Host 合成分段和最终画面；不能沿用旧 offscreen surface 的 PPA 命中率基线。

```sh
clang++ -std=c++23 -O1 -g -fsanitize=address,undefined -DMICROPIXEL_MODEL_TESTING \
  -Iguest guest/apps/blocks/blocks_model.cpp guest/apps/blocks/blocks_model_test.cpp \
  -o build/blocks_model_test
ASAN_OPTIONS=detect_leaks=0 build/blocks_model_test
```
