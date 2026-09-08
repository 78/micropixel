# Guest code

Guest 是独立于芯片平台的 Wasm/AOT 应用侧代码。目录按用途划分：

```text
guest/
├── abi/          # Runtime Host ABI、唯一允许的 import 清单
├── runtime/      # startup 与 typed SDK → C ABI binding
├── sdk/          # Restricted C++23 typed Public API
├── apps/sdk-demo/    # 可导航的 SDK 功能演示应用
├── apps/snake/   # 完整产品应用及其 metadata、素材
├── apps/blocks/  # 触控俄罗斯方块产品应用
├── apps/tilt/    # 加速度计控制的 100 关滚球迷宫
├── apps/maze-evil/ # Direct Surface 全屏软渲染的 2.5D 射击游戏（体感 + 触摸）
└── tests/        # P4 Runtime/SDK conformance
```

普通开发者只实现标准 `int main()`。`runtime/startup.cpp` 在执行 C++ 初始化和 `main()` 前检查
核心 ABI，并导出内部入口 `__micropixel_start`；`runtime/` 集中负责 Public SDK 到 C ABI
的转换。Public SDK 头文件不直接包含 ABI 头。

Runtime binding 按能力拆分，新增实现应放入对应模块：

| 实现 | 职责 |
| --- | --- |
| `service_binding.hpp/.cpp` | Service 打开与调用、错误映射、wire 字节操作 |
| `panic.cpp`、`system.cpp` | 诊断与日志、时钟、随机数、语言和启动参数 |
| `storage.cpp`、`timers.cpp` | KV 存储与 Timer 生命周期 |
| `audio.cpp`、`devices.cpp` | 音频资源与播放、设备枚举及 Sensor/GPIO/Haptics/Power |
| `display_context.cpp` | Graphics/Input 信息缓存与共享坐标契约 |
| `graphics.cpp` | Renderer 信息、字体、纹理和更新批次 |
| `direct_surface.cpp`、`raster_resources.cpp` | HostSurface/GuestSurface 缓冲区所有权、Raster 资源上传与绘制记录 |
| `application.cpp` | 事件循环与 wire 事件解码 |
| `scene_graph.cpp` | Scene 状态与增量提交 |

各能力持有自身 Service 缓存。Graphics binding 共用 `display_context` 的 Service 缓存；
DirectSurface 模块持有缓冲区忙碌状态，Application 解码释放事件后通过内部函数通知它。
Sensor 句柄表保留在设备模块内。内部头只服务于 Runtime，不进入 Public SDK。

[`apps/sdk-demo/`](apps/sdk-demo/) 是 SDK 用法和真机手工检查的统一入口。它只生成一个 Bundle，运行后可从
同一界面进入 Timer/Clock/Log、Input/Random、Storage、Resource/Atlas、Audio 和 Devices/Hardware 页面。每项能力的
实现位于命名明确的独立 CPP，AI 可以按功能直接定位；Renderer 由首页和所有页面共同使用，不再维护
单独的静态绘图程序。

`tests/conformance/` 保留 Event、Timer/Clock、Renderer、退出语义、watchdog 和 Service 边界
验收。历史 S3 Guest、独立 benchmark 和编译失败样例已经移除；需要这类测试时按当前接口重写。
完整产品应用 [`apps/snake/`](apps/snake/)、[`apps/blocks/`](apps/blocks/) 和
[`apps/tilt/`](apps/tilt/) 与 Demo 独立构建。[`apps/maze-evil/`](apps/maze-evil/) 不走 Scene，而是向
`HostSurface` 提交 `RasterDrawList`（墙/地板/精灵/文字都由 Host kernel 光栅化），
是全屏渲染路径与 `--benchmark` 分段统计的验收载体。

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
python3 tools/micropixel --transport usb run guest/apps/sdk-demo

# 已安装 CLI 时，在包含 app.json 的项目目录中可直接运行：
micropixel --transport usb run
```

该命令默认读取当前目录的 `app.json`，完成 development 构建、停止当前 Guest、
安装、启动和日志跟随；`Ctrl-C` 不会停止设备上的 App。只需部署并启动时使用 `micropixel run --no-follow`。
连接设备的 `run`/`app install` 会读取设备芯片并自动选择 AOT target。只做本地产物时可单独使用
`micropixel build`；离线 `micropixel package` 必须显式传入 `--aot-target riscv32-ilp32f` 或
`--aot-target xtensa`。

`build`/`package`/`run`/`app install` 采用与 Ninja 相同的增量规则：产物旁有 `*.stamp.json` 记录上次的
构建参数和输入清单（`app.json`、sources、项目内头文件、编译器发现的传递依赖和工具链文件、资源、
`sfx.json`、翻译文件、`guest/{sdk,runtime,abi}` 和生成器脚本）；参数与清单一致且没有输入比产物新时
直接复用，输出 `Package unchanged, reusing`。这一整包检查只比 mtime。

需要重建时，CLI 在输出目录的 `obj/<编译配置摘要>/` 中复用独立 `.o`，只编译源码或所包含头文件
内容变化的 translation unit，然后重新链接 Wasm、生成 AOT。每个 `.o` 旁有 Clang 生成的 `.d`
依赖文件和 `.json` 缓存记录；依赖包含系统与生成头文件，内容摘要避免生成头文件原样重写引发无谓
编译。编译参数、编译器或其配置变化会切换缓存；同名源文件按完整路径区分。

仓库内 App 默认缓存于 `build/apps/<app>/obj/`，外部项目缓存于项目自己的 `build/obj/`，
单源文件构建默认使用 `build/guest-p4/obj/`；`--output-dir` 同时改变产物和缓存目录。
终端会显示本次编译与复用的 object 数量。删除 `obj/` 可清理缓存，`--force` 会绕过 object 和整包
缓存，强制重新编译打包。

`APP_LIST` / `app.list` 现在带 Catalog SHA-256。`run`/`app install` 在本地 Bundle 的 `appId`、大小和 digest
与已装版本一致时跳过整包上传，返回 `already_installed`；旧固件没有该字段时仍完整安装。`--force` 同时绕过
这层短路径。`run` 即使包没变也会 `APP_STOP` 再 `APP_START`。

完整产品基线仍可使用：

```sh
bash tools/p4.sh build-host
bash tools/p4.sh build-apps
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT
```

`flash-apps` 明确替换 App Store，并写入五个示例 App；不再提供会把任意 Bundle 直接写入
分区的独立公开脚本。单 App 开发安装走 USB Local Control 或 Remote Control 的正常安装事务。

`micropixel build` 默认使用 `development` profile，保留 Wasm 调试信息和 AOT 调用栈；
`micropixel package` 和 `micropixel app install` 默认使用 `release`。Release 使用 Clang `-Oz` 和精简的
AOT 调用栈，但继续保留软件越界检查与内存诊断。需要显式选择时使用
`--profile development|release|size`。链接器只允许 [`abi/allowed_imports.txt`](abi/allowed_imports.txt)
列出的 Runtime import，拼写错误或未授权 import 会在构建阶段失败。

所有模式都启用 Wasm bulk memory：`memcpy`/`memmove`/`memset` 直接降为 `memory.copy`/`memory.fill`，
由 Host AOT 变成原生块拷贝。注意 Guest 用 `-ffreestanding` 编译，编译器不会把手写字节循环自动识别成
memcpy；整帧拷贝、清屏和大块搬运请显式调用 `memcpy`/`memset`。

`micropixel build|package|run|app install --unchecked-memory` 让 wamrc 关闭线性内存越界检查
（`--bounds-checks=0`），并在 Bundle 的 AOT section 置 `UNCHECKED_MEMORY` 位。这样的 Guest 不再被沙箱
隔离，产品 Host 拒绝加载；只有以 `CONFIG_MICROPIXEL_ALLOW_UNCHECKED_AOT=y` 构建的开发 Host 放行，用途是
用同一个 App 做 A/B，量出 bounds-check 的真实开销，再决定是否为本地信任 Bundle 提供正式通道。

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
`af07c787ac6f7d1d20555f97ddc184f5fc13731a` 和 AOT format v6，不是 `wamrc 2.4.3` 版本字符串。
上游 WAMR 2.4.3 至 2.4.5 生成的 AOT v5 不能用于当前固件。

项目自有 C/C++ 代码遵循
[Google-based C++23 代码风格](../docs/development/code-style.zh-CN.md)。项目正式名称为 MicroPixel，
namespace、ABI 前缀和内部入口统一使用 `micropixel`。
