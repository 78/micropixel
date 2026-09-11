# Guest 构建与 Bundle

[English](README.md) · [快速入门](sdk/QUICKSTART.zh-CN.md) · [发布应用](sdk/PUBLISHING.zh-CN.md)

Guest 是独立于芯片平台的 Wasm/AOT 应用代码：`sdk/` 提供公共 C++ API，`runtime/` 负责协议转换，
`abi/` 定义 wire，`apps/` 提供示例，`tests/` 存放 conformance 用例。

## Runtime

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

## 应用清单

使用 `micropixel init` 生成 `app.json` 与 `src/main.cpp`。已有 `app.json` 时拒绝覆盖。
新增编译单元后更新 `sources`；运行时资源通过 `asset_manifest` 声明，音效参数只写在 `audio/sfx.json`。

`version` 必须是规范的 `major.minor.patch` 字符串（最多 31 ASCII 字节），不允许前导零、预发布或
构建后缀。打包器将它写入 Bundle JSON 元数据，Host 读取为 `package_version`；`schema_version` 仍为 1，
它描述清单格式，与应用版本无关。旧清单不填 `version` 仍可构建，旧包读取为空版本，不推定其版本号。
旧的二进制 metadata 模式（`--legacy-metadata-v1`）不携带应用版本。

新版本由用户在设备或 Console 确认安装和运行；当前不进行自动安装调度。发布流程见[发布应用](sdk/PUBLISHING.zh-CN.md)。

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

## 安装与兼容

`run` / `app install` 比较已安装 Bundle 的 App ID、大小和摘要；一致时跳过上传。
`--force` 强制重新构建和上传。`run` 始终重新启动应用。

AOT 使用固定 MicroPixel WAMR fork 和 format v6，不能替换为上游 AOT v5。
产物按架构分开打包；连接设备时自动选择目标，离线打包必须显式指定 `--aot-target`。

Guest 支持受限 C++23 与动态内存，不支持 OS 标准库、exception、RTTI、线程或 WASI import。
具体接口和内存边界见 [SDK](sdk/README.zh-CN.md)。

源码风格见 [C/C++ 规范](../docs/development/code-style.zh-CN.md)，
资源音频格式见 [音频规范](../docs/development/game-audio.zh-CN.md)。
