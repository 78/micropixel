# Firmware 目标架构

本文定义 MicroPixel Firmware 自有代码的目标架构和渐进迁移边界。WAMR、ESP-IDF、LVGL、managed components
和板级厂商驱动继续作为外部依赖，不因本次重构修改其内部结构。

## 1. 目标

Firmware 使用对象表达所有权、生命周期和可替换边界，而不是以增加 class 数量为目标：

- `app_main()` 只创建并运行 `FirmwareApp`；
- `FirmwareApp` 是唯一组合根，负责平台、设备服务、长驻 Runtime 和 System Shell 的构造顺序；
- ESP-IDF、FreeRTOS、LVGL 和 WAMR 的裸句柄由 move-only RAII 类型持有；
- Runtime 依赖显式注入的服务，不读取隐式全局单例；
- Guest C ABI 是薄适配层，只验证参数、查找上下文并转发调用；
- 可恢复失败使用 `std::expected<T, Error>`，C/ESP-IDF 边界再转换为 ABI status 或 `esp_err_t`；
- 实时路径使用固定容量队列、数组和对象池，不发生隐式扩容；
- exception 和 RTTI 保持关闭。

## 2. 分层

```text
app_main (ESP-IDF C entry)
    └── FirmwareApp (composition root)
        ├── Platform
        │   ├── Metalio-Claw4 board/display/input/audio implementation
        │   └── Null implementation
        ├── Host UI
        │   ├── SystemShell
        │   └── App Hall / Status Layer / SystemGestureRouter
        ├── Device Services
        │   ├── GraphicsBackend / GraphicsService
        │   ├── InputBackend / InputService
        │   ├── AudioBackend / AudioService
        │   └── RandomBackend / RandomService
        ├── Runtime
        │   ├── AppRuntime（长驻 WAMR，全局最多一个 AppSession）
        │   ├── AppSession（Bundle/module/instance/exec-env/GuestContext）
        │   ├── ABI adapter / ServiceRegistry / ServiceEndpoints
        │   ├── Bundle reader / AotPackage
        │   ├── ResourceService / BitmapDecoder / BitmapStore
        │   ├── TimerService / StorageService
        │   ├── WamrRuntime / LoadedModule / GuestInstance
        │   └── EventQueue / TouchEventBridge
        └── Conformance hooks（仅测试配置）
```

目录同层不表示领域对象平行。`device/` 是硬件无关的能力契约，`platform/` 是这些契约的板级实现，Runtime
通过 `DeviceServices` 使用它们。核心依赖方向为 `Runtime -> Device contracts <- Platform`；`FirmwareApp` 是唯一
同时知道 Platform 和 DeviceServices 的组合根。Runtime 与 Device 都不得 include `platform/`，Platform 也不得
include Runtime。异步输入通过显式 sink/token 或事件队列返回上层。

## 3. 所有权

| 资源 | 所有者 | 表达方式 |
| --- | --- | --- |
| Firmware 生命周期 | `FirmwareApp` | 栈上唯一对象 |
| WAMR 全局初始化 | `AppRuntime` 内的 `WamrRuntime` | Host 生命周期内一次初始化、move-only RAII |
| AOT package buffer/mapping | `AotPackage` | move-only RAII |
| module、instance、exec env | `AppSession` | 分离的 move-only RAII，数量为 0..1 |
| Guest session 编排 | `AppSession` / `GuestContext` | Session 边界 + 按值组合 services |
| App Hall 与系统 UI | `SystemShell` / Platform `SystemUiBackend` | Host 原生对象，不属于 Guest |
| Bitmap 解码与配额 | `BitmapDecoder` / `BitmapStore` | RAII buffer + 固定容量 slot |
| FreeRTOS queue/semaphore/task | 对应服务 | typed RAII wrapper |
| ESP timer | `TimerService` 固定槽 | generation handle + RAII cleanup |
| NVS handle | `StorageService` | move-only RAII |
| LVGL/display/touch 状态 | 板级 Platform 对象 | 单一实例的私有成员 |
| Touch sink 注册 | 显式 binding token | 先解绑，再等待 inflight 结束 |

裸指针默认 non-owning。指针跨越异步边界时，必须由 binding/token、任务 join 或 shutdown protocol 证明生命周期。

## 4. 并发

- 每个任务拥有自己的可变状态；跨任务数据使用固定容量消息传递；
- ISR 只记录最小 POD 状态并唤醒任务，不调用 Runtime 或 LVGL；
- shutdown 顺序由所有者反向执行：停止接收、唤醒/停止 worker、join、释放队列和底层句柄；
- mutex/critical section 由 scope guard 或封装对象配对，避免分支遗漏 release；
- 不用 detached task 隐藏所有权；必须明确任务何时与 Firmware 同寿命，何时可停止和回收。

## 5. 错误模型

内部接口优先返回具有领域错误类型的 `std::expected`：

```cpp
enum class LoadError {
    kInvalidPackage,
    kOutOfMemory,
    kRuntimeRejected,
};

[[nodiscard]] std::expected<LoadedModule, LoadError> LoadModule(const AotPackage& package);
```

以下边界不改变外部签名：

- `app_main()` 保持 ESP-IDF 规定的 C entry；
- `micropixel_runtime_*` 保持 C ABI 名称和 status code；
- ESP-IDF callback、FreeRTOS task entry、LVGL callback 保持框架规定的函数形状；
- WAMR 的 handle 和 callback 只出现在 Runtime/Adapter 层。

错误在首次获得完整上下文的位置记录一次，避免每层重复打印。只有调用方能恢复的错误才返回 `expected`；违反内部
不变量或 ABI 安全检查失败按现有 fault policy 处理。

## 6. 迁移顺序

1. 建立 C++23、clang-format、clang-tidy 和完整 P4/S3 构建基线；
2. 建立 `FirmwareApp` 组合根，缩减 `app_main()`；
3. 抽取通用 pthread/FreeRTOS/NVS/WAMR RAII 类型；
4. 将 Engine 内的 package、module、instance、exec-env 生命周期拆为 `AppRuntime` / `AppSession`；
5. 让 `GuestContext` 接收显式设备服务依赖；
6. 把 Metalio-Claw4 图形、输入、音频的文件级状态移入板级对象；
7. 将 `micropixel_runtime_*` 收敛为参数验证和转发；
8. 建立 Host 原生 System Shell，使 Guest 结束后返回大厅而不是重启；
9. 完成命名迁移、静态检查、全量构建和设备回归。

每一步必须独立可编译、可烧录，并保持已发布 Guest ABI 和 bundle 格式不变。除非单独决策，不进行一次性重写。

## 7. 当前落地状态

截至 2026-08-23，上述迁移已经落入默认 Firmware 产品路径：

- `app_main.cpp` 只在栈上创建并运行 `FirmwareApp`；pthread 配置、属性和 join 生命周期由组合根中的
  RAII 类型管理；
- CMake 为每个配置只选择一组 Metalio-Claw4 或 Null Platform 源码；`ConfiguredPlatform()` 返回持有板级状态的
  单一对象，`FirmwareApp` 再把四个 Device backend 显式注入 `DeviceServices`，并把 System UI backend 注入
  `SystemShell`；
- 一次性 `Engine` 已拆为长驻 `AppRuntime` 与单次 `AppSession`：WAMR 只初始化一次，Bundle mapping、module、
  instance、exec-env 和 GuestContext 在正常退出、Trap 或失败路径中统一逆序销毁；并发入口明确拒绝第二个
  Session；
- `FirmwareApp` 不再在 Guest 结束后调用 `esp_restart()`；产品路径由 `HostController` 在显式的 Hall / Foreground
  状态间转移，并让 System Shell 保持运行。大厅扫描最多三个 64 KiB 对齐的 Bundle，未运行卡片只 mmap 各自的
  压缩 PNG 封面，首次逐行解码为 218×218 RGB888 PSRAM thumbnail，后续返回大厅按 Bundle 来源键复用；挂起时
  用一份瞬时 720×720 RGB888 capture 在 CPU 上生成 218×218 RGB888 最后一帧 thumbnail 后立即释放全尺寸 source。
  点击原卡片时 Host 直接恢复 retained Guest
  view，再给同一 Session 投递 Resume event；点击另一卡片先销毁旧 Session 再启动新 Session；
- 顶部下滑先协作暂停 Guest，再打开全屏半透明状态层；亮度由板级背光 PWM 控制，音量由 Host mixer 的
  master gain 控制。FPS 开关只启用小型 Host 最终合成蒙层，其中帧率统计可见 Guest commit，CPU 使用率
  由双核 idle run-time delta 推导；
- `AotPackage`、`WamrRuntime`、`LoadedModule`、`GuestInstance` 和 `GuestContextBinding` 是 move-only 或
  scope-bound RAII 类型，错误路径与正常路径使用同一套逆序清理；
- Timer、Resource、Storage 的内部业务接口返回 `ServiceResult<T>`；pointer/length、out parameter 和 C
  status 的转换集中在 `runtime/abi/guest_abi.cpp`；
- 原 2300 行级 Metalio-Claw4 图形实现已经拆成 `BoardHardware`、`CommandStream`、`RetainedScene`、
  `RetainedSurface`、`DirtyRegionCoalescer` 和独立的 `Gt911Input`；`platform.cpp` 持有板级状态并只负责初始化顺序与
  板级绘制操作，Host System UI 与 Guest Graphics 分别通过独立 adapter 接入各自契约；
- `ResourceService` 只负责任务调度，JPEG/PNG 生命周期以及 bitmap/offscreen-surface handle、可变性和
  PSRAM 配额分别归 `BitmapDecoder`、`BitmapStore`；Guest PNG 使用 libpng 逐行直接写最终 ARGB8888
  PSRAM buffer，不创建整图 inflate 临时副本或再做一次整图通道转换，以免与 DSI framebuffer scanout
  争抢 PSRAM 带宽；surface update frame 由 Graphics backend 合并每个
  backing Bitmap 的 damage，在 commit 的同一个显示锁临界区统一通过 `RetainedScene` 失效并唤醒一次
  compositor；touch 转换、统计和绑定生命周期归 `TouchEventBridge`，
  `GuestContext` 回到 session façade；
- ABI 目录收敛为七个文件，ABI v1 保持七个 Core imports。固定容量 `ServiceRegistry` 通过独立 Endpoint 路由 Timer、Storage、
  Resource、Graphics、Input 和 Audio；typed Guest SDK 缓存 Service handle，未发布的旧专用 imports
  已移除；
- 原先同时声明 Bundle、WAMR、watchdog、diagnostics 和 ABI 的 `host_bridge.h` 已删除；`runtime/abi/`、
  `bundle/`、`resources/`、`services/`、`wamr/` 各自暴露窄接口，Runtime 根目录只保留 session 编排对象；
- Event、Timer、Resource、Bitmap 和 Audio Voice 继续使用协议上限决定的固定容量队列、数组或对象池，
  实时路径没有引入无界容器；
- 48-byte Event 以 `service_id + event_id` 扩展；Graphics CommandBuffer 通过通用 `service_submit`
  channel 保留一次一帧的批量提交，call/submit 热路径不分配堆内存或重复查询设备信息。

验证基线：

- Firmware 源码与依赖分别由 `device/`、`runtime/`、`platform/`、`conformance/` 子目录 manifest 维护；
  顶层 `main/CMakeLists.txt` 只负责聚合和注册 ESP-IDF component；
- `main/README.md` 展开到每个文件并记录边界，架构检查禁止 Runtime/Device include Platform 以及 Platform
  include Runtime；
- ESP32-P4 product、conformance 和 Null Platform 三种配置均纳入编译基线；通用 P4 conformance 覆盖
  Service open/call/submit、Event 和非法指针；
- Demo 与 Snake Bundle 用于 Graphics、Input、Storage、Resource、Random 和 Audio 的真机集成检查。

## 8. 明确禁止

- 为“更 OOP”建立深继承树、万能 Manager 或 Service Locator；
- 让 `FirmwareApp` 吸收设备服务的具体业务方法；
- 用全局可变状态代替依赖注入；
- 用裸 `new/delete` 或无界标准容器承担实时资源；
- 在 C ABI 中暴露 C++ class、`std::expected` 或标准库 ABI；
- 修改第三方源码以迎合本项目格式和命名规则。
