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
├── apps/showcase/ # 四个轻量 Showcase App 的共享实现
├── apps/{tap-counter,color-lab,pixel-sketch,orbit-pad}/
│                 # 用于多 App 大厅与 Guest SDK 验收的四个独立 Bundle
└── tests/        # P4 Runtime/SDK conformance
```

普通开发者只实现标准 `int main()`。`runtime/startup.cpp` 在执行 C++ 初始化和 `main()` 前检查
核心 ABI，并导出内部入口 `__micropixel_start`；`runtime/sdk.cpp` 集中负责 Public SDK 到 C ABI
的转换。Public SDK 头文件不直接包含 ABI 头。

[`apps/demo/`](apps/demo/) 是 SDK 用法和真机手工检查的统一入口。它只生成一个 Bundle，运行后可从
同一界面进入 Timer/Clock/Log、Input/Random、Storage、Resource/Atlas、Audio 和 Devices/Hardware 页面。每项能力的
实现位于命名明确的独立 CPP，AI 可以按功能直接定位；Renderer 由首页和所有页面共同使用，不再维护
单独的静态绘图程序。

`tests/conformance/` 保留 Event、Timer/Clock、Renderer、退出语义、watchdog 和 Service 边界
验收。历史 S3 Guest、独立 benchmark 和编译失败样例已经移除；需要这类测试时按当前接口重写。
完整产品应用 [`apps/snake/`](apps/snake/) 和 [`apps/blocks/`](apps/blocks/) 与 Demo 独立构建。

所有游戏音效使用 `apps/<game>/audio/sfx.json` 作为唯一参数源，并在正式 Bundle 构建中执行感知分析门禁。
事件层级、重复暴露、跨游戏对齐和真机 A/B 流程见
[游戏音频设计与感知校准规范](../docs/development/game-audio.zh-CN.md)。
录制的 BGM、对白和长音效使用 asset manifest 的 `ogg_opus` 格式；Host 内置 micro-opus 解码，App Bundle
只携带压缩 Ogg，不需要打包 WAV 或 Guest codec。

日常 App 开发由 `micropixel` 直接读取项目的 `app.json`。Manifest 用 `title` 表达 App Hall 中的用户可见名称，
用唯一的 `sources` 数组列出所有 C++ translation unit，并用 `threading` 声明 `none`（默认）或
`shared-memory`；不再声明屏幕 profile 或重复的单数 `source`。当前 SDK 与集成 App 均使用 `none`，
生成非共享 Wasm linear memory，使 WAMR 通过 `memory.grow` 按需扩展。Bundle 会携带该声明，Host 在加载时
将它与 AOT target-info 的 multi-thread 特征交叉校验。
SDK 初始化时根据物理屏幕建立短边为 720 的逻辑坐标；App 通过 `RendererInfo` 判断当前宽高和方向，
并对不支持的布局显式 `Assert`。`localization`、
`asset_manifest` 和 `audio/sfx.json` 是生成 Catalog、资源绑定、音效 profile、Wasm/AOT 与 Bundle 的
唯一输入，不需要为每个 App 编写 build 脚本：

```sh
python3 tools/micropixel --transport usb run guest/apps/demo

# 已安装 CLI 时，在包含 app.json 的项目目录中可直接运行：
micropixel --transport usb run
```

该命令默认读取当前目录的 `app.json`，完成 development 构建、停止当前 Guest、
安装、启动和日志跟随；`Ctrl-C` 不会停止设备上的 App。只需部署并启动时使用 `micropixel run --no-follow`。
连接设备的 `run`/`app install` 会读取设备芯片并自动选择 AOT target。只做本地产物时可单独使用
`micropixel build`；离线 `micropixel package` 必须显式传入 `--aot-target riscv32-ilp32f` 或
`--aot-target xtensa`。

完整产品基线仍可使用：

```sh
bash tools/p4.sh build-all
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT
```

`flash-apps` 明确替换 App Store，并写入七个示例 App；不再提供会把任意 Bundle 直接写入
分区的独立公开脚本。单 App 开发安装走 USB Local Control 或 Remote Control 的正常安装事务。

`micropixel build` 默认使用 `development` profile，保留 Wasm 调试信息和 AOT 调用栈；
`micropixel package` 和 `micropixel app install` 默认使用 `release`。Release 使用 Clang `-Oz` 和精简的
AOT 调用栈，但继续保留软件越界检查与内存诊断。需要显式选择时使用
`--profile development|release|size`。链接器只允许 [`abi/allowed_imports.txt`](abi/allowed_imports.txt)
列出的 Runtime import，拼写错误或未授权 import 会在构建阶段失败。

Guest 使用 wasi-sdk 33 的 no-exception libc++ profile。常用 header-only STL、动态容器和
`new/delete` 由统一 CLI 配置并按引用裁剪；App 不需要选择或链接独立 STL 模块。OS 相关标准库、
exception、RTTI、Guest thread 和 WASI import 仍不属于受支持能力，具体边界见
[Guest C++ SDK](sdk/README.md#guest-stl-profile)。

动态 STL 使用可增长的 Wasm linear memory。当前 P4 与 S31 的单 Guest 策略上限均为 8 MiB；Host 在启动
App 时会根据最大连续 PSRAM 块下调实际上限，后续按需增长也必须保留自身安全水位。Host 管理的
Texture/offscreen surface 不占 Guest C++ heap，也不预留固定累计配额；每次实际分配都根据当时可用 PSRAM
动态准入。小游戏因此只占实际工作集，大游戏在设备仍有余量时可以继续加载资源。

Guest 代码不得直接依赖 ESP-IDF 或具体开发板。需要访问设备能力时，应经 typed SDK 和
[Runtime Host ABI](abi/README.md) 进入 Host。当前 Public API、错误策略和待冻结事项见
[Guest C++ SDK](sdk/README.md)。`Application` 是可发现的 capability façade；`app.xxx()` 返回
copyable Service View，Service 创建的 Host Resource 才使用 move-only RAII。

Guest AOT 的兼容性基线是 MicroPixel WAMR fork commit
`482b17e07fc46e80ffd23e5290871d42c49748e7` 和 AOT format v6，不是 `wamrc 2.4.3` 版本字符串。
上游 WAMR 2.4.3 至 2.4.5 生成的 AOT v5 不能用于当前固件。

项目自有 C/C++ 代码遵循
[Google-based C++23 代码风格](../docs/development/code-style.zh-CN.md)。项目正式名称为 MicroPixel，
namespace、ABI 前缀和内部入口统一使用 `micropixel`。
