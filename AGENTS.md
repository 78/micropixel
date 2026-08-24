# MicroPixel AI 项目导航

本文件对整个仓库生效，用于帮助 AI 快速建立正确的项目模型。它是导航和操作约束，不复制所有
设计细节。代码、ABI header、可执行测试和下方链接的专项文档是更具体的事实来源。

## 一句话理解项目

MicroPixel 是运行在 ESP32-P4 + Metalio-Claw4 上的 WebAssembly 应用运行时：Host 基于
ESP-IDF 6.1 和 WAMR 2.4.3 AOT，Guest 使用受限 C++23 SDK，通过稳定的 Service ABI 访问图形、
输入、音频、存储和资源，不直接依赖芯片或板级 SDK。

当前产品基线：

- 硬件：ESP32-P4 + Metalio-Claw4；
- Host：ESP-IDF 6.1，一个长驻 `AppRuntime`，同时最多一个 Guest `AppSession`；
- Guest：Wasm32 + RISC-V 32-bit AOT，单线程事件模型；
- 分发：Bundle v1 + 只读 `app_store` 分区；
- 系统 UI：Host 原生 App Hall、Status Layer、系统菜单和系统手势；
- 集成 App：Blocks、Snake 和 Demo。

## 先读哪里

根据任务只读相关文档：

- 项目入口、环境和常用命令：[`README.md`](README.md)；
- 产品边界、分层和发布基线：[`docs/design/architecture.zh-CN.md`](docs/design/architecture.zh-CN.md)；
- Firmware 文件职责：[`firmware/espressif/main/README.md`](firmware/espressif/main/README.md)；
- Guest 目录与构建：[`guest/README.md`](guest/README.md)；
- Public C++ API：[`guest/sdk/README.md`](guest/sdk/README.md)；
- Guest–Host wire 协议：[`guest/abi/README.md`](guest/abi/README.md) 和
  [`guest/abi/micropixel_abi.h`](guest/abi/micropixel_abi.h)；
- C/C++ 格式、命名、所有权和错误策略：[`docs/development/code-style.zh-CN.md`](docs/development/code-style.zh-CN.md)；
- 游戏音频：[`docs/development/game-audio.zh-CN.md`](docs/development/game-audio.zh-CN.md)；
- 真机烧录：[`docs/development/flashing.zh-CN.md`](docs/development/flashing.zh-CN.md)；
- 贡献和提交前检查：[`CONTRIBUTING.md`](CONTRIBUTING.md)。

## 架构速览

```text
app_main
  └─ FirmwareApp                         # 唯一组合根
      ├─ Platform
      │   ├─ Metalio-Claw4                # display/input/audio/system UI
      │   └─ Null                          # 硬件无关编译基线
      ├─ DeviceServices                    # 硬件无关契约
      ├─ AppRuntime
      │   └─ AppSession (0..1)
      │       ├─ Bundle / WAMR module / instance / exec env
      │       ├─ GuestContext / Service endpoints
      │       └─ Event / Timer / Storage / Resource
      └─ HostController / SystemShell       # App Hall、暂停/恢复、系统 UI
```

固定依赖方向：

```text
Runtime -> Device contracts <- Platform
```

- `device/` 定义硬件无关契约；
- `platform/` 实现契约，不依赖 Runtime；
- Runtime 只通过注入的 `DeviceServices` 访问设备；
- `FirmwareApp` 是唯一同时知道 Platform、Device 和 Runtime 的组合根；
- Public SDK 不直接暴露 C ABI，`guest/runtime/sdk.cpp` 负责 typed SDK 到 wire 的 lowering。

Guest 只使用七个 Core imports：`abi_version`、`log_write`、`clock_now`、`event_wait`、
`service_open`、`service_call` 和 `service_submit`。新能力优先扩展 Service method/channel/event，不要
轻易新增 Core import。

## 目录定位

```text
firmware/espressif/main/
  device/                         # Host 硬件无关能力
  platform/                       # 板级实现；metalio-claw4 与 null
  runtime/                        # WAMR、Session、Bundle、ABI adapter、Host Services
  host_ui/                        # System Shell 模型和持久化
  host_controller.*               # App Hall 与 Session 编排
guest/
  abi/                            # wire 格式、ID、allowed imports
  runtime/                        # Guest startup 与 SDK lowering
  sdk/                            # 应用可包含的 Public C++ API
  apps/{blocks,snake,demo}/       # 集成 App
  tests/conformance/              # Guest/Host 边界验收
tools/                            # 构建、打包、分析、烧录和回归脚本
docs/                             # 跨模块长期文档
build/                            # 本地生成产物，不提交
```

## 修改时必须保持的边界

- Guest SDK、Guest App 和 ABI 不得依赖 ESP-IDF、LVGL 或 Metalio-Claw4 类型。
- 不在 C ABI 中暴露 C++ class、STL 类型、vtable、`std::expected` 或 Host 指针。
- 已发布的 Service/method/channel/event/capability/opcode ID 不得改义或复用。
- 所有跨 ABI pointer/length、handle、generation、所属 Guest 和容量都由 Host 验证。
- Guest 是单线程事件模型；不在 App 中引入线程、mutex、系统调用或直接硬件访问。
- Host 实时和跨任务路径使用固定容量队列、数组或对象池，不隐式扩容，不使用 detached task。
- 有身份的资源使用 move-only RAII 或显式 shutdown protocol；析构只做 best-effort cleanup。
- ISR 只记录最小 POD 状态并唤醒任务，不调用 WAMR、Guest 或 LVGL。
- exception 和 RTTI 保持关闭。不使用裸 `new/delete` 承担实时资源所有权。
- Host 拥有设备主音量。Guest 只提供每个音效的 `volume_per_mille`，不得定义 App master
  或对所有音效再做统一衰减。
- 游戏音效的波形、频率、时长、包络和响度只写在 `guest/apps/<game>/audio/sfx.json`，
  不在 C++ 中维护第二份参数。
- 系统手势、App Hall、状态栏、亮度和设备主音量属于 Host，不做成 Guest App。

## 常用任务路由

| 任务 | 优先定位 |
|---|---|
| 修改 Public Guest API | `guest/sdk/` → `guest/runtime/sdk.cpp` → 必要时再改 `guest/abi/` 和 Host endpoint |
| 修改 wire/Service | `guest/abi/` + `firmware/espressif/main/runtime/abi/` + conformance/negative tests |
| 修改 Host 业务能力 | `device/` 契约 + Runtime service；板级差异放 `platform/` |
| 修改应用大厅/状态层 | `host_ui/`、`host_controller.*`、Metalio-Claw4 system UI backend |
| 修改图形热路径 | Graphics Service、Guest graphics engine、display/compositor；保持边界验证 |
| 修改 Blocks/Snake | 对应 `guest/apps/<app>/`；同时运行该 Bundle 的正式构建 |
| 修改音效 | `audio/sfx.json` + 分析器测试 + App Bundle 构建 + 真机 A/B |
| 修改 Bundle/App Store | `tools/build_app_bundle.py`、`tools/build_app_store_image.py`、Host bundle reader |
| 烧录或排查真机 | `docs/development/flashing.zh-CN.md`；先用 MAC 确认目标设备 |

## 构建与验证

环境基线：ESP-IDF 6.1，已通过 `export.sh` 设置 `IDF_PATH`；配置 `WASI_SDK_PATH`/`WASI_CLANG`
和与 Host 匹配的 `WAMRC`。不要在未激活 ESP-IDF 的 shell 中判断 Host 构建失败。

最小验证要与变更风险匹配：

```sh
# Guest ABI/SDK 基线
bash tools/build_guest_p4.sh

# ESP32-P4 Host 产品基线
bash tools/build_p4_baseline.sh

# System Shell + Blocks + Snake + Demo + App Store 集成
bash tools/build_system_shell_p4.sh

# Firmware 格式；首次完整 clang-tidy 需要 --configure
bash tools/check_firmware_style.sh --format-only

# Host 回归
bash tools/tests/test_firmware_host.sh

# 音频分析器和所有游戏 manifest
python3 -m unittest tools.tests.test_analyze_sfx -v

# 正式 App Bundle
bash tools/build_blocks_bundle.sh
bash tools/build_snake_bundle.sh
bash tools/build_demo_bundle.sh

# Shell 语法
bash -n tools/*.sh
```

修改什么就验证什么：

- 文档变更：检查相对链接和 `git diff --check`；
- Guest SDK/ABI：至少运行 Guest 构建和相关 conformance；
- Firmware 代码：至少运行相关 Host test、格式和 P4 Host 构建；
- System Shell、Bundle 或集成 App：运行 `build_system_shell_p4.sh`；
- 音频：运行分析器 unit tests、对应 Bundle 构建和真机试听；
- 硬件行为：记录目标板型和验收结果，但不提交 MAC、串口日志或设备标识。

## 生成物、第三方与安全

- `build/`、`artifacts/`、`managed_components/`、生成的 `sdkconfig`、AOT/Wasm/Bundle/Flash 镜像和报告
  是本地产物，不直接编辑，不提交。
- 音频生成头文件和资源 pack 由构建脚本生成；修改其源 JSON、素材或生成器。
- `firmware/espressif/components/wasm-micro-runtime/` 是固定 commit 的 WAMR fork/submodule。除非任务明确
  要求更新 fork，不要把其源码复制回主仓库。
- 不为了统一格式而改动第三方源码。新依赖必须核对许可证并更新
  `THIRD_PARTY_NOTICES.md`。
- 不提交密钥、令牌、私钥、个人绝对路径、设备序列号、MAC、原始串口日志或一次性性能数据。

## AI 工作流程

1. 先运行 `git status --short`，区分用户未提交变更与当前任务，不覆盖、重置或顺手提交无关文件。
2. 先查找现有契约、相似实现和测试，再选择最小正确变更面。
3. 保持上述 Host/Guest/ABI/Platform 边界；如果需要突破，必须先更新架构决策和回归基线。
4. 使用与风险成比例的测试验证，不以“能编译”代替协议、生命周期或真机行为验收。
5. 交付时说明行为变化、关键文件、已运行命令、结果和仍需的真机/人工验收。

## 完成定义

一项变更完成时应同时满足：

- 职责放在正确的 Host、Guest、ABI、Device 或 Platform 层；
- 无意外 wire/Bundle/持久化格式变化，或已配套版本和兼容测试；
- 所有权、并发、容量、错误路径和 shutdown 顺序有明确语义；
- 相关自动测试和构建通过；
- 文档与当前行为一致；
- 没有夹带用户的无关变更、生成物、设备标识或敏感信息。
