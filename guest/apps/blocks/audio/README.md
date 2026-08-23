# Blocks 音效感知评估

本目录实现项目级[游戏音频设计与感知校准规范](../../../../docs/development/game-audio.zh-CN.md)；
下文只记录 Blocks 的事件层级和设备校准细节。

`sfx.json` 是 Blocks 音效的唯一参数源。构建脚本先按 ESP32-P4 Host 的真实规则合成 16 kHz PCM，
执行感知评估，再生成 Guest 使用的 `blocks_sfx_profiles.hpp`。运行时代码与分析器不再各维护一份参数。

评估器输出以下工程指标：

- `event A`：整段事件经过 A-weighting 和设备频响后的能量级，用于比较长短不同的提示音；
- `relative/target`：相对快速落地音的层级及目标层级；
- `repeat A`：按该事件最大触发频率折算的一秒重复暴露，用于约束移动/软降疲劳；
- `peak`：数字峰值，防止混音削波和过强瞬态；
- `HF ratio`：2 kHz 以上能量占比，作为尖锐度代理；
- `transient`：相邻采样最大跳变，捕获方波边沿和 click；
- `score`：将层级误差、尖锐度、峰值、瞬态和重复暴露合并后的 0–100 工程评分；
- `gain hint`：达到目标层级所建议的线性音量倍率。

常规构建会生成 `build/apps/blocks/sfx-report.json` 并带 `--check` 执行。任何音效越过
`sfx.json` 中的边界，构建都会失败：

```sh
bash tools/build_blocks_bundle.sh
```

独立运行、导出可试听 WAV 和执行算法回归：

```sh
python3 tools/analyze_sfx.py \
  --manifest guest/apps/blocks/audio/sfx.json \
  --device-profile firmware/espressif/main/platform/metalio-claw4/audio/perceptual_profile.json \
  --report build/apps/blocks/sfx-report.json \
  --write-wavs build/apps/blocks/sfx-wavs \
  --check
python3 -m unittest tools.tests.test_analyze_sfx -v
```

设备频响文件使用按频率递增的 `[Hz, dB]` 点，分析器在对数频率轴上插值。当前仓库文件明确标记为
`uncalibrated-flat`；它可完成相对数字感知比较，但不能声称绝对声压或听力安全。使用校准麦克风播放扫频并
填写实测增益后，可通过环境变量让构建使用新的设备曲线：

```sh
MICROPIXEL_SFX_DEVICE_PROFILE=/absolute/path/measured-speaker-response.json \
  bash tools/build_blocks_bundle.sh
```

目标层级来自本机实际试听反馈：移动/软降比快速落地约低 18 dB，自然落地约低 15 dB；启动、消行和
Game Over 因为是多音事件，允许具有更高的整段能量。评分用于稳定地权衡和防止回归，最终舒适度仍以
同一台设备上的 A/B 试听为准。
