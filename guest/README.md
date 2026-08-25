# Guest code

Guest 是独立于芯片平台的 Wasm/AOT 应用侧代码。目录按用途划分：

```text
guest/
├── abi/          # Runtime Host ABI、唯一允许的 import 清单
├── runtime/      # startup 与 typed SDK → C ABI binding
├── sdk/          # Restricted C++23 typed Public API
├── apps/demo/    # 可导航的 SDK 功能演示应用
├── apps/snake/   # 完整产品应用及其 metadata、素材
├── apps/blocks/  # 触控俄罗斯方块产品应用
└── tests/        # P4 Runtime/SDK conformance
```

普通开发者只实现标准 `int main()`。`runtime/startup.cpp` 在执行 C++ 初始化和 `main()` 前检查
核心 ABI，并导出内部入口 `__micropixel_start`；`runtime/sdk.cpp` 集中负责 Public SDK 到 C ABI
的转换。Public SDK 头文件不直接包含 ABI 头。

[`apps/demo/`](apps/demo/) 是 SDK 用法和真机手工检查的统一入口。它只生成一个 Bundle，运行后可从
同一界面进入 Timer/Clock/Log、Input/Random、Storage、Resource/Atlas 和 Audio 页面。每项能力的
实现位于命名明确的独立 CPP，AI 可以按功能直接定位；Renderer 由首页和所有页面共同使用，不再维护
单独的静态绘图程序。

`tests/conformance/` 保留 Event、Timer/Clock、Renderer、退出语义、watchdog 和 Service 边界
验收。历史 S3 Guest、独立 benchmark 和编译失败样例已经移除；需要这类测试时按当前接口重写。
完整产品应用 [`apps/snake/`](apps/snake/) 与 Demo 独立构建。

所有游戏音效使用 `apps/<game>/audio/sfx.json` 作为唯一参数源，并在正式 Bundle 构建中执行感知分析门禁。
事件层级、重复暴露、跨游戏对齐和真机 A/B 流程见
[游戏音频设计与感知校准规范](../docs/development/game-audio.zh-CN.md)。

P4 应用构建入口是：

```sh
bash tools/build_demo_bundle.sh
bash tools/build_snake_bundle.sh
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT
```

`flash-apps` 明确替换 App Store，并写入 Blocks、Snake 和 Demo；不再提供会把任意 Bundle 直接写入
分区的独立公开脚本。单 App 开发安装走 Remote Control 的正常安装事务。

默认 `development` profile 保留 Wasm 调试信息和 AOT 调用栈；发布构建显式设置
`MICROPIXEL_GUEST_PROFILE=release`。链接器只允许 [`abi/allowed_imports.txt`](abi/allowed_imports.txt)
列出的 Runtime import，拼写错误或未授权 import 会在构建阶段失败。

Guest 代码不得直接依赖 ESP-IDF 或具体开发板。需要访问设备能力时，应经 typed SDK 和
[Runtime Host ABI](abi/README.md) 进入 Host。当前 Public API、错误策略和待冻结事项见
[Guest C++ SDK](sdk/README.md)。`Application` 是可发现的 capability façade；`app.xxx()` 返回
copyable Service View，Service 创建的 Host Resource 才使用 move-only RAII。

项目自有 C/C++ 代码遵循
[Google-based C++23 代码风格](../docs/development/code-style.zh-CN.md)。项目正式名称为 MicroPixel，
namespace、ABI 前缀和内部入口统一使用 `micropixel`。
