# Juicy Blocks

Juicy Blocks 是与 Juicy Snake 同系列的 720×720 触控俄罗斯方块 Guest App。界面沿用近黑终端 HUD、
关卡强调色、位图按钮和固定容量运行时结构。棋盘由 4 个 Host PSRAM offscreen surface 组成，
活动块移动只上传发生变化的格子，不再按帧重建整棵绘图对象树。

```text
blocks/
├── app.json
├── main.cpp
├── blocks.hpp
├── blocks_common.hpp
├── blocks_model.hpp/.cpp       # 10×20 棋盘、7-bag、Hold、Ghost 和计分
├── blocks_game.hpp/.cpp        # 状态机、触控、计时和存档
├── blocks_renderer.cpp         # HUD、4 条带 surface、格子光栅化与脏区同步
├── blocks_audio.cpp            # 由统一 profile 驱动的短音效
├── audio/                      # 音效参数、感知约束和校准说明
├── blocks_app.cpp              # 资源加载与事件循环
├── blocks_model_test.cpp       # 可原生运行的规则回归
└── assets/
    ├── manifest.json
    ├── source/generate_ui_assets.py
    └── blocks_*.png
```

构建 App Bundle：

```sh
python3 tools/micropixel package guest/apps/blocks
```

兼容脚本 `bash tools/build_blocks_bundle.sh` 只转发到同一条 manifest 驱动的命令。

触控操作覆盖整个 720×720 屏幕：任意位置点击旋转、水平拖动、慢速下拖软降、快速下划硬降、上划换块；
轻点 HOLD 换块，轻点左上角标题区域暂停。从 HOLD 或标题区域起手的滑动仍按游戏手势处理，不会被按钮截断。

游戏每消除 10 行提升一级。自动下落周期使用连续曲线
`P(level) = 750 ms / ⁴√(1 + 8.58 × (level − 1))`：1 级为 750 ms、12 级约 240 ms、24 级约 200 ms。
曲线没有人为速度上限，后期仍会持续变快，但相邻等级的变化会自然放缓；内部使用微秒精度，避免高等级因
整数毫秒取整出现速度平台。软降和硬降仍允许熟练玩家主动加快节奏。

渲染器把 10×20 棋盘按每 5 行拆为 4 个 `300×150 RGB888` offscreen surface。Guest 缓存 200 个
visual-cell code，逻辑变化后重新合成活动块、Ghost 和落定棋盘，只对 code 改变的 `30×30` 格子调用
`StreamingTexture::Update()`。每次 `SyncPlayfield()` 用一个 `TextureUpdateBatch` 包住全部写入；Host
按 surface 合并脏格，commit 时统一 invalidate 并只唤醒一次 compositor。合并后的活动块区域通常超过
4096 pixels，可进入 ESP32-P4 PPA RGB888 image SRM 路径，不再把逐格 CPU fallback 过程暴露到屏幕。
普通横移/旋转不提交新的 `Frame`；HUD、Hold/Next 和 overlay 仅在状态变化时提交。
离屏 buffer 数量和脏格统计只保留在 Host 诊断日志中，不占用发布版 HUD。

音效参数只维护在 `audio/sfx.json`。`tools/analyze_sfx.py` 逐采样复现 Host 合成器，结合可替换的设备
频响计算 A-weighted 事件能量、重复暴露、尖锐度代理、瞬态和层级评分；构建会生成报告及 Guest 头文件，
并在感知约束回归时失败。算法、WAV 导出和扬声器校准方法见 `audio/README.md`。

抓屏 Host 还可从 USB Serial/JTAG 注入同路径触控并立即抓图：

```sh
python3 tools/drive_p4_touch.py /dev/cu.usbmodem1101 build/captures/blocks-playing.png \
  --reset --gesture tap:360:352 --gesture tap:560:230 \
  --gesture swipe:520:300:610:300:120:3 --gesture flick:560:250:560:520:100
```

规则回归可原生运行；设备集成还应检查启动日志中的四条
`created offscreen surface ... 300x150`，并确认纯横移日志没有新的 `touch submit`。真机还应出现
`offscreen frame ... unions=... ppa-eligible=... stage=...`；持续横移时每个 frame 只发布一次 refresh，
且常规移动的 `ppa-eligible` 应大于 0：

```sh
clang++ -std=c++23 -O1 -g -fsanitize=address,undefined -DMICROPIXEL_MODEL_TESTING \
  -Iguest guest/apps/blocks/blocks_model.cpp guest/apps/blocks/blocks_model_test.cpp \
  -o build/blocks_model_test
ASAN_OPTIONS=detect_leaks=0 build/blocks_model_test
```
