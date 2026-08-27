# Snake 音效感知评估

本目录实现项目级[游戏音频设计与感知校准规范](../../../../docs/development/game-audio.zh-CN.md)；
下文只记录 Snake 的事件层级和背景旋律处理。

`sfx.json` 是 Snake 音效和背景旋律的唯一参数源。构建脚本按照 ESP32-P4 Host 的 16 kHz
整数混音规则生成 PCM，执行感知评估，再生成 Guest 使用的 `snake_sfx_profiles.hpp`。运行时与分析器
不会分别维护两套波形、频率、包络或音量参数。

Guest 不再定义 App master，设备整体音量交由 Host 系统状态栏控制。高频进食事件的内容响度仍按最大
触发频率单独校准，不再通过 Guest 统一衰减来达到舒适度边界。

背景旋律在 JSON 中分成 `bgm_a`、`bgm_b` 两个八音短句；JSON 的 delay 按 140 BPM 模拟整句暴露，
运行时仍由游戏等级决定音符间隔。Start Game 和 Continue 共用 Blocks 的 Start Game 提示音；
其余事件包括四种食物、升级和 Game Over。

报告中的主要指标为：

- `short/target`：最响 50 ms 窗口的 RMS 及绝对目标，防止整组源波形电平过低；
- `relative`：短时 RMS 相对普通进食声的层级；
- `event A`：经过 A-weighting 与设备频响后的整段累计能量；
- `repeat A`：结合最大触发频率的一秒重复暴露；
- `peak`、`HF ratio`、`jump/peak`：分别约束峰值、尖锐度与相对跳变；
- `score`：综合层级误差、重复暴露、尖锐度、峰值和瞬态后的 0–100 工程评分；
- `gain hint`：达到目标相对层级的建议线性音量倍率。

普通进食声是跨游戏参考音，50 ms RMS 固定为 `-14 dBFS`；BGM 单音窗口低 12 dB，确保可听且不会盖过操作音。
启动、金色食物、升级和失败通过更长的旋律取得辨识度，而不是依赖更尖锐或更响的波形。构建会生成
`build/apps/snake/sfx-report.json` 并在越界时失败：

```sh
python3 tools/micropixel package guest/apps/snake
```

独立生成报告和试听 WAV：

```sh
python3 tools/analyze_sfx.py \
  --manifest guest/apps/snake/audio/sfx.json \
  --device-profile firmware/espressif/main/platform/boards/metalio-claw4/audio/perceptual_profile.json \
  --report build/apps/snake/sfx-report.json \
  --write-wavs build/apps/snake/sfx-wavs \
  --check
```

当前设备 profile 标记为 `uncalibrated-flat`，因此算法能可靠防止数字响度和音色回归，但不能替代
同一台设备上的 A/B 试听，也不能代表校准后的绝对声压。完成扬声器扫频实测后，可通过
`MICROPIXEL_SFX_DEVICE_PROFILE` 指向测量曲线，无需修改 APP。
