# 游戏音频设计与感知校准规范

本文是所有 MicroPixel 游戏 Guest 的音频开发规范。新游戏和新增音效必须遵守本文；已有游戏修改音效时
也必须继续通过相同的自动分析和真机验收。目标是让音效参数可审查、可计算、可回归，并保持不同游戏在
同一设备上的主观响度连续性。

算法指标是工程代理，不是校准声压测量，也不能单独证明“听起来舒服”。自动门禁负责发现数字响度、
尖锐度、瞬态和重复暴露回归，最终判断必须包含目标设备上的 A/B 试听。

## 1. 强制目录和单一参数源

每个使用合成音频的游戏必须具有以下结构：

```text
guest/apps/<game>/
├── audio/
│   ├── sfx.json       # 唯一音效参数源，必须提交
│   └── README.md      # 游戏特有的层级选择、事件语义和试听说明
└── <game>_audio.cpp   # 只消费生成的 ToneSpec，不硬编码音色参数
```

波形、频率、时长、`volume_per_mille`、Attack、Release 和音符 Delay 必须写在 `audio/sfx.json`。
运行时代码不得另行维护同一组常量。允许运行时根据游戏状态选择 profile、改变 BGM 节拍或截取前缀，
但音符本身仍来自生成的 `ToneSpec`。

生成的 `<game>_sfx_profiles.hpp`、分析报告和试听 WAV 都属于构建产物，写入 `build/apps/<game>/`，
不得提交到源码目录。新游戏可从 [game-sfx.template.json](game-sfx.template.json) 开始。

## 2. 构建门禁

游戏的正式 Bundle 构建脚本必须在编译 Guest 前执行：

```sh
python3 tools/analyze_sfx.py \
  --manifest guest/apps/<game>/audio/sfx.json \
  --device-profile firmware/espressif/main/platform/metalio-claw4/audio/perceptual_profile.json \
  --emit-cpp-header build/apps/<game>/assets/<game>_sfx_profiles.hpp \
  --report build/apps/<game>/sfx-report.json \
  --check
```

`--check` 不得从发布构建中省略。运行时代码必须包含生成头文件，并将其中的 `volume_per_mille`
原样传给 Host；构建参数 `MICROPIXEL_SFX_DEVICE_PROFILE` 可以替换设备频响，但不能绕过检查。

每次修改分析器、JSON schema 或游戏音效，还必须运行：

```sh
python3 -m unittest tools.tests.test_analyze_sfx -v
bash tools/build_<game>_bundle.sh
```

## 3. 事件清单和响度层级

开始调音前，先列出所有能触发声音的事件，包括启动、频繁移动、普通确认、稀有奖励、升级、失败和 BGM。
每个事件都必须在 JSON 中声明实际最坏情况下的 `max_rate_hz`，不能使用平均触发率。每秒可能出现三次
以上的事件必须启用 `check_repetition_exposure`。

每个游戏选择一个“主要操作或普通奖励确认声”作为 `reference_effect`，其
`target_relative_db` 为 `0.0`。推荐的初始层级如下，最终值可按玩法微调：

| 事件类别 | 相对参考声的建议范围 |
|---|---:|
| 高频移动、拖动或软降 tick | -20～-12 dB |
| 普通操作/奖励确认 | 0 dB |
| BGM 代表短句 | +3～+5 dB，且重复暴露应低于高频事件 |
| 启动、状态切换、稀有奖励 | +4～+7 dB |
| Game Over | +5～+7 dB |
| 升级或极低频重大反馈 | +8～+10 dB |

`event A` 衡量整段事件能量，因此较长的旋律即使单个音符很轻，也可能高于短促确认声。不能只比较
`volume_per_mille`。相同硬件 profile 和系统音量下，不同游戏的同类事件原则上应控制在 ±3 dB 内；
超过时必须在游戏的 `audio/README.md` 说明设计原因。

Metalio-Claw4 上的 Guest 不得定义 App master，也不得对所有音效再做一层统一衰减。每个音效的
相对响度由 `volume_per_mille` 表达；设备的整体音量由 Host 系统音量统一控制。

## 4. 舒适度约束

默认 limits 使用模板中的当前项目基线：

- 数字峰值 `peak_dbfs_max: -12.0`，给同时播放和设备处理保留余量；
- 高频事件重复暴露 `step_exposure_dbfs_max: -39.0`；
- 2 kHz 以上能量比例 `high_frequency_ratio_max: 0.10`；
- 相邻采样跳变 `transient_delta_dbfs_max: -30.0`；
- 相对层级允许误差 `relative_tolerance_db: 3.0`。

频繁反馈优先使用 Sine 或 Triangle，并设置可感知但不过长的 Attack/Release。Square 和 Noise 不是禁止项，
但不能用于频繁事件；一旦造成高频比例或瞬态越界，应先更换波形或放缓包络，而不是只降低 Master。

同时发声数量必须满足 Host 的 8 synth voices 上限。一个 profile 当前最多包含 8 个 Tone；超过 8 音的
旋律应拆成多个可分析短句。延迟值必须反映代表性播放节奏，避免分析器把实际重叠音错误当成顺序音，
或把实际顺序音错误当成和弦。

## 5. 调音顺序

1. 使用真实事件清单和最大触发率建立 JSON，不先追求评分。
2. 导出现状报告和 WAV，保存到 `build/` 作为本地基线。
3. 先消除峰值、尖锐度、瞬态和重复暴露违规，再调整相对层级。
4. 使用 `gain hint` 估算幅度变化；幅度翻倍约增加 6 dB，但修改后必须重新分析。
5. 与至少一个现有游戏的同类事件比较 `event A`，避免切换游戏时整体突变。
6. 构建正式 Bundle，烧录目标设备，连续触发高频事件并确认没有 audio command dropped。
7. 在相同设备音量、握持方式和环境中 A/B 试听；至少检查安静环境、正常环境和连续操作三种场景。
8. 将最终目标层级和有意保留的例外写进游戏的 `audio/README.md`。

## 6. 验收清单

新游戏合入前必须全部满足：

- [ ] `audio/sfx.json` 是运行时所有音色参数的唯一来源；
- [ ] 正式构建使用项目设备 profile、生成头文件与报告，并带 `--check`；
- [ ] 所有效果没有 analyzer violation，评分不作为唯一通过条件；
- [ ] 高频事件使用最坏触发率并检查重复暴露；
- [ ] 与 Blocks/Snake 的同类事件完成跨游戏层级比较；
- [ ] 生成头文件的回归测试覆盖该 manifest 的关键 profile；
- [ ] 正式 Bundle 构建通过，真机没有丢命令或 voice exhaustion；
- [ ] 目标设备完成 A/B 试听，游戏特有取舍记录在 `audio/README.md`。

设备 profile 当前若标记为 `uncalibrated-flat`，报告只能用于相对数字比较。得到校准麦克风扫频数据后，
应更新或替换设备频响曲线，再复跑所有游戏；任何情况下都不要把这些指标表述为绝对声压或医学听力安全结论。
