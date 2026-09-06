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

`bash tools/tests/test_firmware_host.sh` 每次都会执行全部 Host 测试，但会复用未变化的测试二进制。
编译缓存位于 `build/host-tests/`，通过编译器解析依赖并检查源码、头文件内容、编译参数和工具链环境；
修改这些输入后自动重编译。Bundle reader 的多组集成测试也共用该缓存。需要强制重编译时使用
`HOST_TEST_REBUILD=1 bash tools/tests/test_firmware_host.sh`，或删除 `build/host-tests/`。

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
