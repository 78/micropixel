# MicroPixel AI 项目入口

本文件对整个仓库生效。新任务先建立下方项目模型，再按任务读取相关文档；代码、ABI header 和
可执行测试是具体行为的事实来源。这里不维护完整接口、文件树或历史实验。

## 项目模型

MicroPixel 是 Espressif MCU 上的 WebAssembly 应用运行时。Host 使用 ESP-IDF 6.1 和固定 commit 的
WAMR fork（AOT v6）；Guest 使用受限 C++23 SDK，单线程事件循环，同时最多一个 AppSession。
ESP32-P4 + Metalio-Claw4 是产品 profile，S31/Mosaico 与三款 S3 板型为 preview。
应用以 Bundle v1 分发，BundleFS v2 负责 App Store 的写时复制与掉电恢复。

依赖方向固定为 `Runtime → Device contracts ← Platform`，`FirmwareApp` 是唯一组合根。
Host 管理硬件、应用生命周期和系统 UI；Guest 通过 Service ABI 使用能力，不感知具体板型。

## 按任务找入口

Host 路径均相对 `firmware/espressif/main/`。

| 任务 | 代码入口 | 优先阅读 |
|---|---|---|
| 环境、构建和项目概览 | `tools/` | [README](README.md) |
| 架构与职责划分 | `device/`、`runtime/`、`platform/` | [架构](docs/design/architecture.zh-CN.md)、[Firmware 导航](firmware/espressif/main/README.md) |
| Guest 应用与 SDK | `guest/apps/`、`guest/sdk/`、`guest/runtime/` | [Guest 构建](guest/README.md)、[SDK](guest/sdk/README.md) |
| wire / Service | `guest/abi/`、`runtime/abi/`、`runtime/services/` | [ABI](guest/abi/README.md)、[ABI header](guest/abi/micropixel_abi.h) |
| 大厅、状态层、系统手势 | `host/ui/`、`host/controller/` | [Firmware 导航](firmware/espressif/main/README.md) |
| 图形与性能 | `platform/graphics/`、`platform/lvgl/`、`runtime/graphics/` | [图形诊断](docs/development/graphics-performance.zh-CN.md) |
| Bundle / App Store | `runtime/bundle/`、`runtime/bundlefs/`、`tools/` | [BundleFS](docs/design/bundlefs.zh-CN.md) |
| 游戏音效 | `guest/apps/<app>/audio/sfx.json` | [音频规范](docs/development/game-audio.zh-CN.md) |
| 板级适配、烧录、真机排错 | `platform/boards/` | [烧录指南](docs/development/flashing.zh-CN.md)、[S3 适配](docs/development/esp32-s3-box-3-bring-up.zh-CN.md) |

C/C++ 修改遵循[代码风格](docs/development/code-style.zh-CN.md)；其他专题从[文档索引](docs/README.md)查找。

## 必须保持的边界

- Guest SDK/App/ABI 不依赖 ESP-IDF、LVGL 或板型类型，不引入线程、mutex、系统调用或直接硬件访问。
- Public SDK 到 wire 的转换集中在 Guest Runtime；新能力优先扩展 Service method/channel/event，
  不轻易增加七个 Core imports。已发布 ID 不得改义或复用，C ABI 不暴露 C++ 布局、STL 或 Host 指针。
- Host 验证跨 ABI 的 pointer/length、handle、generation、所属 Guest 和容量，不能只信任 SDK 校验。
- Host 实时和跨任务路径用固定容量队列、数组或对象池；不隐式扩容、不使用 detached task。
  资源用 move-only RAII 或显式 shutdown protocol，析构只做 best-effort cleanup；不用裸 new/delete
  承担实时资源所有权。exception 和 RTTI 关闭。
- ISR 只记录最小 POD 并唤醒任务，不调用 WAMR、Guest 或 LVGL。Guest 热路径不同步输出大段日志。
- App Hall、状态层、系统手势、亮度和设备主音量归 Host；Guest 不增加 App master 或统一音量衰减。
  游戏音色参数只写在 `audio/sfx.json`，不在 C++ 维护第二份。

## 工作与验证

1. 先运行 `git status --short`，保留用户未提交改动，不覆盖、重置或夹带无关文件。
2. 查找现有契约、相似实现和测试，选择最小正确变更面。突破架构边界前先更新设计与回归基线。
3. 按下表验证；编译通过不能代替协议、生命周期或硬件行为验收。
4. 交付说明行为变化、关键文件、执行命令与结果，以及仍需的真机/人工验收。文档同步当前行为。

| 变更范围 | 最低验证 |
|---|---|
| 文档 | 相对链接、`git diff --check` |
| Guest SDK / ABI | `bash tools/build_guest_p4.sh` 和相关 conformance |
| Firmware | 相关 Host test、格式检查、`bash tools/p4.sh build-host` |
| 共享 graphics / lvgl / PPA 条件分支 | 另构建 S31 和至少一款 S3 |
| System Shell / Firmware | 相关 Host test、格式检查、`bash tools/p4.sh build-host`；不构建 Guest 或 App Store |
| Bundle / 集成 App | 相关测试和对应正式 Bundle 构建；只有修改 Host 时才追加 `bash tools/p4.sh build-host` |
| 音效 | 分析器 unit tests、对应 Bundle 构建、真机 A/B 试听 |
| 图形性能 | 同时对照 Guest 与 Host 分段采样及可见画面，不只看 CPU% |

Host test 只通过 `bash tools/tests/test_firmware_host.sh` 编译运行，不直接调用 clang++。
格式入口为 `bash tools/check_firmware_style.sh --format-only`；发布或推送前运行
`bash tools/p4.sh test`，更多检查见 [CONTRIBUTING](CONTRIBUTING.md)。

构建前激活 ESP-IDF 6.1 的 export.sh，并配置 WASI SDK 和匹配的 WAMRC；未激活环境不能据此判断
Host 构建失败。新增 Kconfig 符号后检查实际生成配置，避免旧 sdkconfig 无声关闭新代码。
真机操作前读烧录指南，用芯片 MAC 确认目标，不依赖会重枚举的串口名；同一设备只允许一个串口工具占用。

## 仓库卫生

- 修改源 JSON、素材或生成器，不直接编辑/提交 build、artifacts、managed_components、生成的 sdkconfig、
  dependencies.lock.*、资源 pack、AOT/Wasm/Bundle/Flash 镜像或报告。
- WAMR 是固定 commit 的 fork/submodule，只有任务明确要求时才更新；不复制其源码回主仓库，
  不为统一格式改第三方代码。新依赖核对许可证并更新 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES.md)。
- 不提交密钥、令牌、个人绝对路径、设备标识、MAC、原始串口日志或一次性性能数据。
- `guest/apps/mario/` 是 gitignore 排除的本地基准，可能不存在，不能成为仓库必需依赖。
