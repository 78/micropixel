# Contributing

感谢参与。提交改动前请确保变更保持 Guest ABI 与具体芯片/板卡实现解耦，并遵循
[项目代码风格](docs/development/code-style.zh-CN.md)。

## 基本检查

```sh
git submodule update --init --recursive
bash tools/build_guest_p4.sh
bash tools/check_firmware_style.sh --format-only
python3 -m unittest tools.tests.test_analyze_sfx -v
bash -n tools/*.sh
```

涉及固件行为时，还应完成 ESP32-P4 Host 构建和相应真机回归。PR 中请写明测试环境、执行命令和结果；
不要提交串口日志、性能采样、构建目录、固件镜像或设备标识。

## 新游戏音频

新增游戏或修改游戏音效时，必须遵循
[游戏音频设计与感知校准规范](docs/development/game-audio.zh-CN.md)：使用 `audio/sfx.json` 作为唯一
音色参数源，在正式 Bundle 构建中生成运行时头文件和报告并执行 `--check`，完成跨游戏层级比较和目标
设备 A/B 试听。新 manifest 从
[game-sfx.template.json](docs/development/game-sfx.template.json)复制，不能把波形、频率或音量重新硬编码到 C++。

## 新文件与依赖

- 项目自有代码默认采用 Apache-2.0；建议在新源码中使用 `SPDX-License-Identifier: Apache-2.0`。
- 引入第三方代码前确认许可证兼容性，保留原版权/许可声明，并更新 `THIRD_PARTY_NOTICES.md`。
- 第三方数据手册、原理图、截图和二进制素材只提交来源链接；只有明确允许再分发时才可入库。
- 新测试应是可重复、仍由构建或 CI 执行的 conformance/regression test。一次性实验应在外部记录，
  不把原始数据长期放进源码仓库。

请勿提交密钥、令牌、私钥、个人绝对路径、设备序列号、MAC 地址或其他敏感数据。
