# C/C++ 代码风格

本文适用于仓库自有的 Host、Guest SDK、Guest Runtime、示例和工具代码。WAMR、ESP-IDF、
managed components、第三方源码和生成文件遵循其上游风格，不做机械重排。

## 1. 风格定位

本项目采用以下统一 C++ profile：

- 语言标准为 C++23；Host 使用 `gnu++23`，freestanding Guest 使用 `c++23`；
- 格式以 clang-format 的 Google preset 为基线，只把缩进改为 4 空格、行宽改为 120 列；
- exception 和 RTTI 关闭；
- 核心设计使用 RAII、固定容量容器和有界对象池；
- 可恢复业务错误使用 `std::expected` 或与其核心接口兼容的 `Result<T>`；
- C++26 只允许在内部实验目标中局部试用，不进入公共接口。

准确名称是 **Google-based C++23 style**，不是“完全遵循 Google C++”。Google preset 负责可自动执行的
格式基线；本节列出的嵌入式语言和运行时约束负责限定可使用的 C++ 能力。

仓库根目录的 [`.clang-format`](../../.clang-format) 是可自动执行的格式基线。发生冲突时，
已记录的项目约束优先于上游默认值。

检查或格式化改动文件时使用：

```sh
clang-format --dry-run --Werror path/to/file.cpp
clang-format -i path/to/file.cpp
clang-tidy path/to/file.cpp -- -std=c++23 -ffreestanding -Iguest
```

Firmware 使用 ESP-IDF Clang 工具链生成的真实 compilation database，统一执行格式与命名门禁：

```sh
# 首次使用或构建配置变化后生成 product/conformance compilation database
bash tools/check_firmware_style.sh --configure

# 日常完整检查；只检查格式时添加 --format-only
bash tools/check_firmware_style.sh
```

普通 build/flash 不隐式执行格式或静态分析门禁。发布前或推送前统一运行 `bash tools/p4.sh test`；需要
单独检查完整 clang-tidy 时仍使用上面的检查脚本，避免使用 GCC compilation database 时靠删除未知参数
得到不可靠结果。Firmware 的 conformance test hooks 也在检查范围内，只有第三方组件、managed
components 和生成目录排除。

Guest 的命名规则由 `guest/.clang-tidy` 中的 `readability-identifier-naming` 固化。C ABI 约定要求的
`__micropixel_*` 内部导出是函数命名检查的显式例外，普通 C++ API 不得沿用该前缀。

命名直接采用 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html#Naming) 的公开规则：
普通函数使用 `PascalCase`，变量使用 `snake_case`，常量和枚举值使用 `kPascalCase`。Google 同时明确允许
property accessor/mutator 使用 `snake_case`，本项目只在这一公开例外以及标准兼容、C ABI 场景中保留它。

## 2. 格式

- 使用 4 个空格，不使用 Tab；
- 每行不超过 120 列；
- 大括号、换行、指针和引用、短函数及 include 排序遵循 Google preset；
- 不在源码中用手工对齐对抗 clang-format；
- CI 和编辑器使用仓库固定版本的 clang-format，避免版本漂移造成无意义 diff。

示例：

```cpp
namespace micropixel {

class Timer final {
   public:
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void Cancel();

   private:
    uint32_t handle_{};
};

}  // namespace micropixel
```

MicroPixel 是项目正式名称，C++ namespace 统一使用 `micropixel`。板级名称不得进入通用 SDK 类型。

## 3. 命名

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| namespace | `lower_snake_case` | `micropixel::runtime` |
| 类型、enum class | `PascalCase` | `Application`, `ErrorCode` |
| 普通函数、普通方法 | `PascalCase` | `WaitEvent`, `Frame::Present` |
| property accessor/mutator | `snake_case` | `width()`, `set_value()` |
| 变量、参数 | `snake_case` | `error_code`, `elapsed_us` |
| 私有数据成员 | `snake_case_` | `handle_`, `initialized_` |
| 编译期常量 | `kPascalCase` | `kEventQueueCapacity` |
| enum 成员 | `kPascalCase` | `ErrorCode::kInvalidState` |
| 宏、C ABI 常量 | `UPPER_SNAKE_CASE` | `MICROPIXEL_ABI_VERSION` |
| 文件 | `lower_snake_case` | `timer_service.cpp` |

C ABI 导出统一使用 `micropixel_` 前缀。此前的 placeholder 已随正式项目名称确定；ABI 冻结前仍可
进行必要的不兼容演进，冻结后不得因营销名称改变而随意更名。

以下名称保留 `snake_case`，但不能据此扩散到普通项目方法：

- 只返回或设置对象 property 的 accessor/mutator，例如 `score()`、`set_value()`；包含校验、资源转移或其他操作的
  初始化方法仍使用 `PascalCase`；
- 与 `std::expected` 对齐的 `has_value()`、`value()`、`error()`、`value_or()` 和 `unexpected()`；
- 与 range/STL 协议对齐的 `begin()`、`end()`、`c_str()` 和运算符；
- C ABI、Wasm 导出和 user-defined literal 等外部协议规定的名称。

## 4. C++23 规则

优先使用：

- RAII 和 move-only 对象表达 Host 资源所有权；
- value type、`enum class`、`constexpr`、`noexcept`、`[[nodiscard]]`；
- Host 对可恢复业务结果使用 `std::expected<T, Error>`；freestanding Guest 使用
  `Result<T>`，其观察接口兼容 `std::expected<T, Error>` 的常用子集，组合接口按实际需求增加；
- 构造后有效的对象，或由类型明确表达 moved-from/empty 状态；
- `nullptr`、范围清晰的整数类型和显式转换。

禁止或默认不使用：

- exception、RTTI、复杂继承和深层模板元编程；
- 裸 `new/delete` 表达业务所有权；
- Guest 线程、mutex、任意系统调用和直接硬件访问；
- 将 C++ class ABI 暴露为 Host/Guest 边界；
- 用 `bool` 丢失本来需要诊断的失败原因。

Host 可以使用目标工具链提供的 C++23 标准库子集。Guest 使用项目固定的 wasi-sdk no-exception
libc++ profile，可以使用已由 conformance 覆盖的 utility、容器、算法和 RAII 类型；linker 按引用
裁剪未使用实现。Guest 不能假定存在 exception、RTTI、thread、filesystem、locale/iostream、WASI
系统调用或任意未验收标准库能力。Public SDK 不跨 Guest–Host ABI 暴露 STL 类型。

Public SDK 可以使用一层模板检查 `Application::Run(handler)` 的 handler 签名；不得让应用显式填写
模板参数，也不得把模板错误替代清晰的事件概念。常用路径应能从 `sdk/micropixel.hpp` 单头文件编译，
业务型 `Result<T>` 则按需显式包含。

### 4.1 C++26 实验边界

C++26 代码必须放在单独的内部实验 target/source 中，并满足：

- 公共头文件仍能在纯 C++23 下编译；
- 不把 C++26 类型、模板、inline 实现或语义带入 Guest SDK、C ABI、持久化格式和 wire format；
- 不成为默认构建或受支持平台的必要依赖；
- 进入正式路径前降级为 C++23，或通过单独决策整体提升项目语言基线。

### 4.2 Public SDK 对象语义

Public 类型的名称、复制能力和所有权必须共同表达其类别：

| 类别 | 规则 | 示例 |
| --- | --- | --- |
| Service View | 由 `app.xxx()` 按值返回，轻量且 copyable，没有独立资源身份 | `Log`、`Clock`、`Timers` |
| Resource | 工厂创建，默认 move-only，拥有 typed Host handle | `Timer`、`Texture`、`StreamingTexture` |
| Value | copyable，只包含自有数据，不在析构时调用 Host | `Duration`、`TimePoint`、typed event |

Module 是编译、链接或部署概念，不作为运行时对象类别。Service View 的 class 名不添加机械的
`Service`、`Manager` 或 `View` 后缀；通过获取位置、copyability 和文档表达语义：

```cpp
Clock clock = app.clock();             // Service View
TimePoint now = clock.Now();           // Value
Timers timers = app.timers();          // Service View
Timer timer = timers.After(100_ms);    // Resource
```

强类型单位不得同时保留无单位整数构造。`Duration` 只能通过 `_us/_ms/_s` literal 或带单位 factory
构造；`TimePoint` 的非零值只能来自 `Clock` 或 typed event。`Run(handler)` template 必须用
`static_assert`/`requires` 给出事件层诊断，不能让错误签名最后表现为难以定位的模板展开失败。

`Application` 是 capability façade。它可以公开 `renderer()`、`audio()`、`input()` 等稳定顶层
能力入口和唯一的 `Run(handler)` 事件编排入口，但不能吸收 Service 或 Resource 的叶子操作。
Timer 只能通过 `app.timers().After/Every()` 创建。优先在 Input service 上增加普通方法，
不新增 Application 的同类叶子操作；优先写 `frame.FillRect(...)`，不新增
`app.DrawRect(...)`。

## 5. 所有权和错误处理

- Host 资源由 move-only SDK proxy 持有，析构自动 release；
- SDK Runtime 和 Host 实时路径的集合默认使用编译期固定容量；Guest App 的普通非实时业务逻辑可以
  使用受支持的动态 STL 容器，但必须接受 Host linear-memory quota 和确定的 OOM policy；当前 P4 产品
  上限为 8 MiB，包含 Guest 静态数据、栈和动态 heap；
- Timer、Texture、Audio Voice、Guest Context 等具有稳定身份的同构资源使用有界对象池；
- 普通 value type 直接按值存储，不为统一形式机械放入对象池；
- 裸指针默认是 non-owning，必须从作用域和类型上看出其有效期；
- 跨 ABI 的 pointer/length、handle、类型、generation 和所属 Guest 必须由 Host 验证；
- 程序错误和不可恢复的 Runtime/ABI 状态在原始调用点记录 operation/status 并 panic；
- 只有调用方能采取不同业务行动的失败才转换为 `expected/Result`，应用不直接处理 ABI 状态码；
- Host 内部使用 `std::expected<T, Error>`；Guest Public SDK 使用 freestanding `Result<T>`；二者不跨越 C ABI；
- Guest `Result<T>` 提供 `has_value()`、`operator bool()`、`operator*`、`operator->`、`value()` 和
  `error()`；错误状态访问值或成功状态访问错误会在调用点 panic/trap，而不是抛异常；
- 组合接口按 Guest 的实际需求增加 `transform`、`and_then`、`or_else`，不得为追求完整标准库兼容
  引入动态分配、异常运行时或大规模模板 machinery；
- 析构函数不得抛出，也不能依赖应用处理 release 失败。

## 6. Public SDK 与 ABI 边界

- `guest/sdk/*.hpp` 是应用可包含的 Public API，不直接包含 `guest/abi/*.h`；
- `guest/runtime/sdk.cpp` 负责 typed SDK 到 C ABI 的 lowering 和错误映射；
- `guest/abi/*.h`、Wasm import module 和 `__micropixel_start` 是 Runtime 内部协议；
- startup 在调用应用 `main()` 前完成核心 ABI 兼容检查；
- Public Event 只暴露 typed payload，不暴露 `data0/data1`、raw handle 或 buffer layout；
- ESP-IDF、WAMR、LVGL 和具体开发板类型不得出现在 Guest Public API。
- 一个 Public C++ 方法不自动对应一个 Wasm import；低频能力优先 lower 到通用 Service 控制面，
  高频/大块数据只有在测量证明需要时才增加专用 transport。新增 import 必须遵守
  [Guest–Host ABI](../../guest/abi/README.md)。

## 7. C 代码

新增 C 代码主要用于 ABI、厂商 API 边界和上游 C 组件适配：

- 使用同一套 Google-based、4 空格、120 列格式；
- 函数和变量使用 `snake_case`，公开符号必须有模块前缀；
- 文件内 `static const` 编译期数据使用 `kPascalCase`；宏和 C ABI 常量使用 `UPPER_SNAKE_CASE`；
- 固定宽度协议字段使用 `uint32_t` 等明确类型；
- ownership、buffer 长度和返回值必须在声明附近说明；
- 不用宏模拟复杂类型系统；能保持 C++ Public API 的逻辑不要重复放入 C 层。

## 8. 变更要求

修改 SDK、ABI 或资源生命周期时，应同时提供：

1. 最小 Guest Demo 功能页或 test；
2. Host/Guest 空目录构建；
3. 与风险匹配的 P4 真机回归；
4. Public API、ABI 文档和 roadmap 的同步更新。
