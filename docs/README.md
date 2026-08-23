# Documentation

- `design/`：架构讨论稿和版本化设计记录；
- `roadmap/`：当前实施路线、阶段边界和里程碑条件；
- `development/`：开发流程与代码规范；
- `hardware/`：板卡和器件的官方来源链接，不分发第三方二进制文档。

设计稿不等于已冻结规范。一次性实验报告、原始串口日志和历史性能数据不在源码仓库中长期维护；
仍需自动回归的行为应写成 `guest/tests/conformance/` 下的可执行测试。

当前实施顺序见 [MicroPixel Application Runtime 开发里程碑](roadmap/development-milestones.zh-CN.md)。
Guest–Host 的 ABI 稳定性、Service 独立演进和 Graphics/Media 数据面规则见
[Guest–Host Service ABI 稳定与演进规范](design/guest-host-service-abi.zh-CN.md)。
所有新游戏的合成音效必须遵循
[游戏音频设计与感知校准规范](development/game-audio.zh-CN.md)，并从通过门禁的
[JSON 模板](development/game-sfx.template.json)开始。
