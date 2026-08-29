# Blocks 音效分析与试听验收

本目录实现项目级[游戏音频设计与验收规范](../../../../docs/development/game-audio.zh-CN.md)；
下文只记录 Blocks 的事件层级和试听细节。

`sfx.json` 是 Blocks 音效的唯一参数源。构建脚本先按 ESP32-P4 Host 的真实规则合成 16 kHz PCM，
执行感知评估，再生成 Guest 使用的 `blocks_sfx_profiles.hpp`。运行时代码与分析器不再各维护一份参数。

Guest 不再定义 App master，历史的 45% 统一衰减已移除；设备整体音量由 Host 系统状态栏控制。

评估器输出以下工程指标：

- `short/target`：最响 50 ms 窗口的 RMS 及绝对目标，负责保证源波形充分使用数字动态范围；
- `relative`：短时 RMS 相对快速落地音的层级；
- `event A`：整段事件经过 A-weighting 后的累计能量，用于暴露分析而非最低响度门禁；
- `repeat A`：按该事件最大触发频率折算的一秒重复暴露，用于约束移动/软降疲劳；
- `peak`：数字峰值，防止混音削波和过强瞬态；
- `HF ratio`：2 kHz 以上能量占比，作为尖锐度代理；
- `jump/peak`：相邻采样跳变相对于事件峰值的比例，捕获方波边沿和 click；
- `score`：将层级误差、尖锐度、峰值、瞬态和重复暴露合并后的 0–100 工程评分；
- `gain hint`：达到目标层级所建议的线性音量倍率。

常规构建会生成 `build/apps/blocks/sfx-report.json` 并带 `--check` 执行。任何音效越过
`sfx.json` 中的边界，构建都会失败：

```sh
python3 tools/micropixel package guest/apps/blocks
```

独立运行、导出可试听 WAV 和执行算法回归：

```sh
python3 tools/analyze_sfx.py \
  --manifest guest/apps/blocks/audio/sfx.json \
  --report build/apps/blocks/sfx-report.json \
  --write-wavs build/apps/blocks/sfx-wavs \
  --check
python3 -m unittest tools.tests.test_analyze_sfx -v
```

分析器只比较数字音频，不能声称绝对声压或听力安全，也不对具体板型和扬声器频响建模。

快速落地是跨游戏参考音，50 ms RMS 固定为 `-14 dBFS`；移动/软降短时层级低 14 dB，自然落地低 8 dB。
启动、消行和 Game Over 依靠时长与旋律取得辨识度，不靠超高瞬时电平。评分用于稳定地权衡和防止回归，
最终舒适度仍以同一台设备上的 A/B 试听为准。
