# Juicy Snake

这是 MicroPixel 的首个完整 Guest 游戏。目录只保留产品源码、Bundle metadata 和最终素材；历史
验收页面、contract 与测试场景已经移除，需要回归时按当时的产品行为重新编写测试。

```text
snake/
├── app.json                  # AppId、显示名称和启动资源
├── main.cpp                  # 最小 Guest 入口
├── snake.hpp                 # 应用入口声明
├── snake_app.cpp             # 资源检查、启动和事件循环
├── snake_common.hpp          # 内部共享常量和轻量类型
├── snake_model.hpp/.cpp      # 独立玩法模型
├── snake_game.hpp/.cpp       # 状态机、输入、计时和存档
├── snake_renderer.cpp        # HUD、棋盘和画面提交
├── snake_effects.cpp         # 粒子、轨迹、弹字和闪光
├── snake_audio.cpp           # 消费生成 profile 的 BGM、音序器和音效
├── audio/                    # JSON 音效参数、感知约束和校准说明
├── gamekit/
│   ├── cyclic_pool.hpp       # 固定容量循环对象池
│   └── swipe_gesture.hpp     # 连续滑动与点击识别
└── assets/
    ├── manifest.json         # 资源、atlas 帧和画布位置的唯一数据源
    ├── snake_*.png           # 最终游戏资源
    └── source/               # 启动图的可编辑 PNG 源文件
```

构建输出统一写入 `build/apps/snake/`：

```sh
python3 tools/micropixel package guest/apps/snake --aot-target riscv32-ilp32f
```

`gamekit/` 仍是 Snake 内部实现；只有第二个游戏出现相同需求且语义稳定后，才移动到公共 SDK。
通用的按钮交互、固定字符串和 retained Scene API 位于 `guest/sdk/`；START/RESTART 使用 SDK 的
`TextButton`，不再携带纯色按钮贴图。

当前 Metalio-Claw4 与 ESP-Mosaico 音频硬件链路均为 16 kHz；Snake 仍根据 `Audio::info()` 使用 Host
实际报告的格式，不把板级采样率写进游戏逻辑。
音色参数只维护在 `audio/sfx.json`，构建时执行项目统一的感知门禁并生成 `snake_sfx_profiles.hpp`；通用规则
见[游戏音频设计与感知校准规范](../../../docs/development/game-audio.zh-CN.md)。

素材构建以 `assets/manifest.json` 为唯一入口。Snake 启动图需要透明背景，因此保留 RGBA PNG；不透明
启动图默认应使用可由 ESP32-P4 硬件解码的 JPEG。launch 封面不接受 raw RGB888/ARGB8888。Food 使用规则
sprite sheet，Burst 使用紧裁 atlas，并通过 `canvas` 与 `canvas_position` 恢复每帧在逻辑画布中的稳定位置。
构建阶段校验 PNG、帧边界和类型顺序，只生成一个 `snake_assets.hpp` 头文件，再写入最终 Bundle
资源区。

Food sprite 的原始 36×36 帧保存在 `assets/source/*_1x.png`。为了让 720×720 逻辑画布缩放到
480×480 物理屏幕后仍能看清图案，发布素材使用最接近 1.2× 的整数帧尺寸 43×43；修改 1× 源素材后运行：

```sh
python3 guest/apps/snake/assets/source/generate_food_sheets.py
```

棋盘背景是一个随 Theme 更新填充色和边框色的 `RoundedRectNode`，不再为纯色背景加载 625×625
纹理；START/RESTART 由 `TextButton` 绘制。Snake 的普通移动使用固定容量 SpriteBatch 和 body ring：尾槽复用为新身体位置，SDK 只序列化事务的净
instance 差量。Food/Burst 通过 Sprite atlas source patch 播放；粒子复用固定池 Batch。震动先提交完成的
Game Container，再在震动期间冻结其子节点，只更新 Container translation；HUD 位于独立 Container，Host 因而可以
捕获一次 Game Container snapshot 并用 PPA/DMA2D 移动，而不会被同时变化的 flash/particle 迫使全量重放。
顶部 HUD 从 `RendererInfo::safe_area_insets()` 读取圆角屏的逻辑边缘内缩：标题保留额外视觉 padding，
Level、Score、Best、状态文字和 Combo 条按右侧 inset 整组左移，不按 Mosaico 板名硬编码布局分支。轻点左上标题所在的顶部 HUD 可暂停；热区覆盖完整标题但不侵入棋盘。
