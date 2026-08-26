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
python3 tools/micropixel package guest/apps/snake
```

兼容脚本 `bash tools/build_snake_bundle.sh` 只转发到同一条 manifest 驱动的命令。

`gamekit/` 仍是 Snake 内部实现；只有第二个游戏出现相同需求且语义稳定后，才移动到公共 SDK。
通用的按钮、固定字符串和 Renderer/Frame helper 已放在 `guest/sdk/`。

当前 P4 / Metalio-Claw4 Mode 1 音频硬件链路固定为 16 kHz；Snake 根据 `Audio::info()` 使用这一格式。
音色参数只维护在 `audio/sfx.json`，构建时执行项目统一的感知门禁并生成 `snake_sfx_profiles.hpp`；通用规则
见[游戏音频设计与感知校准规范](../../../docs/development/game-audio.zh-CN.md)。

素材构建以 `assets/manifest.json` 为唯一入口。Snake 启动图需要透明背景，因此保留 RGBA PNG；不透明
启动图默认应使用可由 ESP32-P4 硬件解码的 JPEG。launch 封面不接受 raw RGB888/ARGB8888。Food 使用规则
sprite sheet，Burst 使用紧裁 atlas，并通过 `canvas` 与 `canvas_position` 恢复每帧在逻辑画布中的稳定位置。
构建阶段校验 PNG、帧边界和类型顺序，只生成一个 `snake_assets.hpp` 头文件，再写入最终 Bundle
资源区。
