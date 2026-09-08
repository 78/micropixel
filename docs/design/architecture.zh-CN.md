# MicroPixel 架构与发布基线

MicroPixel 把应用逻辑放在 WebAssembly Guest 中，把硬件、系统 UI 和资源管理留给 Host。
这种分工让同一个应用源码适配不同开发板，同时由 Host 控制内存、设备访问和失控应用的退出。
本文解释设计边界与取舍；接口用法见 [Guest SDK](../../guest/sdk/README.md)，
实现入口见 [Firmware 导航](../../firmware/espressif/main/README.md)。

## 1. 产品边界

- 产品硬件为 ESP32-P4 + Metalio-Claw4；ESP32-S31 + ESP-Mosaico、ESP32-S3-BOX-3、立创
  SZPI ESP32-S3 和 M5Stack CoreS3 为 preview profile。
- Host 使用 ESP-IDF 6.1 与固定 commit 的 MicroPixel WAMR fork，执行 AOT format v6。
  Guest 使用受限 C++23，不依赖 ESP-IDF、LVGL 或板级 SDK。
- 一个长驻 Runtime 同时最多持有一个 AppSession。Guest 按单线程事件模型运行。
- Bundle v1 封装 AOT、资源和应用元数据；BundleFS v2 管理可写 App Store，P4 为 24 MiB，S31/S3 为 8 MiB。
- App Hall、Status Layer、系统手势、亮度和设备主音量由 Host 管理。

多 Guest 并行、Guest 多线程、Guest Network/Camera Service 和通用 Widget Server 不属于当前基线。
开发版远程安装已经实现，生产级分发仍受第 8 节的发布门槛约束。

## 2. Host 分层

依赖方向固定为：

```text
Runtime → Device contracts ← Platform
```

Runtime 表达应用运行所需的能力，Platform 把板上硬件实现为这些契约。`FirmwareApp` 是唯一同时
知道两者的组合根，负责装配和注入；Runtime 不根据板名分支，Platform 不调用 Runtime。
因此新增板型通常只需新增硬件组合和构建 profile，应用协议与生命周期可以保持一致。

| 层 | 负责什么 | 边界 |
|---|---|---|
| Device | 硬件无关契约、设备身份与共享校验 | 不暴露芯片和驱动类型 |
| Platform | 板级初始化、驱动、总线、显示与音频执行 | 不拥有应用业务或系统页面 |
| Runtime | Session、Service、资源与 Guest 隔离 | 只通过注入的设备契约访问硬件 |
| Host Controller / System Shell | 应用切换、系统交互与电源编排 | 不把系统功能交给 Guest |
| ABI adapter | Guest 内存验证、协议转换与转发 | 业务规则归对应 Service |

Board 只登记初始化成功的能力；Platform 为缺失能力提供 unavailable 实现。这样服务集合可以完整，
设备却不必具备全部硬件。Null Board 用于验证这种依赖边界，不是可烧录的产品替代品。

系统页面、交互和生命周期由 Host 统一管理，分辨率 profile 提供布局，Board 提供显示、亮度和转场能力。
硬件转场可缺省，基本交互仍可工作。App Hall 只保留可见卡片及预取窗口，避免 UI 和封面内存随安装数量
线性增长。板型只消费呈现请求，不读取 Hall 索引或持有页面内部状态。

目录定位见 [Firmware 导航](../../firmware/espressif/main/README.md)，术语见
[硬件分层与命名](firmware-terminology.zh-CN.md)。

## 3. Session 与事件模型

WAMR 随 Firmware 初始化一次，每次启动应用创建一个 Session。Session 统一持有 Bundle mapping、
WAMR module/instance、执行环境和 Guest 资源；正常退出、Trap、启动失败和应用切换均经过逆序清理。
切换应用前必须结束旧 Session，避免设备租用和异步工作泄漏到下一应用。

Guest 的 `Run(handler)` 串行处理 Timer、输入、音频和生命周期事件。一个 handler 执行期间不会插入
另一个 handler，应用因而无需线程同步。代价是长计算会延迟事件处理，必须有 watchdog 和强制停止边界。
周期 Timer 积压时合并通知并保留实际经过时间，应用不能假定每次通知都只经过一个固定周期。

暂停由 Host 等待 Guest 到达 `event_wait` 安全点，不向 Guest 增加 Pause 事件。暂停时冻结应用时钟、
Timer、输入、音频和 watchdog；恢复复用原 Session，首先投递 Resume。Stop 先交给 handler，返回后
退出事件循环。协作停止或安全点等待超过 500ms 时强制停止，保证大厅和电源控制仍能响应。

电源状态独立于大厅/前台状态。休眠先暂停应用、释放显示，再进入平台低功耗；唤醒先恢复硬件和原
Session，超时被停止的应用则回到大厅。选择自动休眠策略的板型中，空闲超时和电源键共用这一流程。
Platform 通过 `Power::GetIdlePowerAction()` 指定空闲时休眠、关机或禁用；Claw4 与 Mosaico 空闲超时
使用完整关机流程，Claw4 手动电源键仍支持休眠。Mosaico 的 GPIO57 仅作开漏关机输出，不作为电源键输入或唤醒源。开机或唤醒所用的同一轮按键
必须释放后才接受新请求，避免误休眠或误关机；入睡被硬件拒绝不能当作成功唤醒。关机先停止应用、
取消远控输入并静音，再交给板级断电能力。OTA 写入期间拒绝休眠和关机。

系统手势由 Host 拦截，不能同时成为 Guest 输入。具体电源策略与验收见
[定时器与空闲功耗](../development/timers-and-idle-power.zh-CN.md)，事件用法见
[Guest SDK](../../guest/sdk/README.md)。

## 4. Guest–Host 边界

Public SDK 提供强类型 C++ 对象，由 Guest Runtime 转换成稳定的 C wire 协议。C ABI 不暴露 C++ class、
STL、vtable 或 Host 指针，避免编译器和内部布局变化影响应用兼容性。

七个 Core imports 提供基础运行与传输，能力通过独立版本的 Service 扩展：

| 通道 | 用途 | 设计原因 |
|---|---|---|
| `service_call` | 有界控制请求和响应 | 保持同步操作简单、可验证 |
| `service_submit` | Scene patch、光栅记录等批量数据 | 摊薄高频跨边界调用成本 |
| `event_wait` | 输入、Timer、完成与生命周期通知 | 让 Guest 在无工作时阻塞 |

Service major 必须相同，Host minor 不低于 Guest 要求。已发布 ID 不得改义或复用；新能力优先扩展
method/channel/event，其次新增 Service，增加 Core import 必须有现有传输不足的证据。
ID、wire 布局和兼容规则集中在 [ABI 文档](../../guest/abi/README.md)与
[ABI header](../../guest/abi/micropixel_abi.h)。

Host 验证所有 pointer/length、handle 类型、generation、所属 Guest 和容量。SDK 的类型安全只能帮助
应用正确使用接口，不能代替 Host 对不可信输入的验证。

设备发现与设备操作分开：目录回答“有什么”，Sensors/GPIO/Haptics 等 Service 管理使用方式。
不透明 DeviceId 独立于枚举位置，parent 表达组合设备；应用依能力选择设备，不依赖物理地址或板名。
GPIO 只暴露板级白名单，打开形成独占租用，释放后恢复安全状态。Sensor 打开才采样，Read 读取最新缓存；
最后一个 handle 释放或应用暂停时停止采样。共享总线统一调度，ISR 只投递最小状态，不执行 Guest 逻辑。

## 5. Graphics 与 Resource

正式版前的 Scene、2.5D 前端与共享资源设计见
[SDK API 重构](sdk-api.zh-CN.md)。以下描述当前实现，迁移验收完成后再替换为新基线。

图形提供两种应用模型，系统 UI 的所有权保持一致：

- **Scene** 保存对象树，适合页面、精灵和局部更新。Guest 提交属性变化，Host 验证后只重绘受影响区域。
- **DirectSurface** 管理整帧缓冲，适合 raycaster 等全屏渲染。默认缓冲由 Host 持有，Guest 提交
  RasterResources 绘制记录；算法和场景判断留在 Guest，逐像素循环在 Host 执行。

Scene 的 Container 表达子树生命周期、局部坐标和继承属性。新路径的 setter 只修改 Guest 状态，
Renderer::Present 统一提交；失败保留待提交状态，删除的 handle 不会复活。可以保存多个场景，
切换发送 keyframe；普通更新提交净差量 patch。Host 用 generation/revision 验证基线。

2.5D / 伪 3D 游戏不走通用浮点网格：Guest SDK 的几何前端（`Raycaster` 等）把地图、相机和
billboard 变成 RasterResources 记录，每像素填充仍由 Host 的 INDEX8 + 光照调色板整数内核完成。
不提供通用浮点网格与逐像素深度缓冲：MCU 没有值得依赖的浮点吞吐。ABI 2.0 的 Graphics 使用原有 Core transport。

布局和输入使用 SDK 的同一逻辑坐标空间，SDK 将其转换为物理坐标后发送。Host 不重复实现应用布局。
应用从 RendererInfo 查询尺寸、安全区和容量，不能靠板名判断；字体使用 Host 提供的语义角色。

Scene 先合成为 App Surface，再由显示后端呈现。支持直接扫描输出的板型可在系统 UI 不可见时绕过
LVGL 的重复合成；系统页面或转场出现时交回 LVGL。交接需要完整同步画面，避免显示旧内容。
多缓冲让合成与显示并行，但已显示或在飞的缓冲不可改写；丢弃中间帧也必须累计 damage，保证最终内容完整。
显示链路与取舍见 [Graphics 性能诊断](../development/graphics-performance.zh-CN.md)。

DirectSurface 存活期间拒绝 Scene submit。Present 后缓冲归 Host，释放前 Guest 不得写入或再次提交。
默认 Host buffer 不映射给 Guest，避免长期保留 Guest 指针；Guest buffer 模式必须声明 pinned memory，
保证线性内存增长不移动基址，其代价是提前占用连续 PSRAM。暂停期间停止扫描并归还在飞缓冲。

Texture 的 Guest 句柄和 Scene 引用独立计数：Guest Reset 只释放自己的引用，仍被 Scene 使用的像素
必须继续存在。可变像素只通过动态纹理快照更新，不存在原地写入的 streaming 纹理。资源加载可在 Host 后台解码，
公开加载调用仍同步等待。动画时间由 Guest 驱动，当前不提供 Host AnimationClip/Track。

## 6. 所有权、并发与错误

有身份的资源使用 move-only RAII 或显式 shutdown protocol。裸指针默认不拥有资源；跨异步边界必须
证明上下文活到工作结束。关闭顺序是停止接收、唤醒 worker、join，最后释放队列和底层句柄；析构只做
best-effort cleanup，不 Panic、不抛异常。

Host 实时与跨任务路径使用固定容量队列、数组和对象池，不隐式扩容、不创建 detached task。
任务核心和优先级集中在 [task_policy.hpp](../../firmware/espressif/main/work/task_policy.hpp)，
后台解码、持久化和日志不得阻塞 Guest 热路径。ISR 不调用 WAMR、Guest 或 LVGL。

Scene 与 Raster 仅在提交或上传入口按需显式分配，失败保留原状态，绘制期间不分配。应用资源随
Session 释放，显示缓冲按显示生命周期管理；具体资源契约见 [ABI](../../guest/abi/README.md)。

Guest 线性内存位于 PSRAM，按需增长，当前策略上限为 8 MiB，并受最大连续块与 Host 安全水位约束。
Host-owned 纹理和 surface 在实际分配时同样检查安全水位，不提前占满理论配额。这样轻量应用能把内存
留给显示、解码和系统交互；应用不能假定理论上限始终可分配。

可处理的业务失败返回 Result；编程错误和 ABI 安全失败进入 panic/fault policy。异常和 RTTI 关闭。
设备主音量归 Host；Guest 只控制单次音效或播放的音量。详细规则见
[代码风格](../development/code-style.zh-CN.md)与[游戏音频规范](../development/game-audio.zh-CN.md)。

## 7. Bundle、能力与权限

Bundle reader 在创建 WAMR instance 前检查格式、hash、范围、对齐和唯一性。当前一个 Bundle 只含一个
AOT section，按 CPU 架构分别构建；安装在写入 App Store 前拒绝缺少 target 元数据或架构不匹配的 AOT。
容器为未来多架构留有空间，但多 AOT 选择尚未启用。

BundleFS 使用写时复制：先写并验证新数据，最后提交新 Catalog，掉电后选择最后一代有效记录。
离散数据块减少连续空洞问题。App Store 重装已安装 App 时先卸载旧版本再安装，只需容纳新版本，
但删除后失败会让该 App 处于未安装状态。Catalog 位于 app_store，擦除系统
NVS 不影响应用。格式、迁移和恢复规则只在 [BundleFS 文档](bundlefs.zh-CN.md)维护。

设备能力回答“能否提供操作”，权限回答“当前应用是否获准操作”。Service 发现和版本协商不等于授权；
缺失能力与权限拒绝必须使用不同错误语义，权限应按动作划分。
当前 requirements section、权限声明与 grant 尚未实现，不能把进入 main 后的 Trap 当作兼容性预检。

## 8. 发布基线

五个集成应用覆盖完整游戏、公开 Service、DirectSurface、传感器和多 Bundle 生命周期。
自动门禁与命令统一见 [CONTRIBUTING](../../CONTRIBUTING.md)，真机流程见
[构建与烧录](../development/flashing.zh-CN.md)。

真机回归必须覆盖：最多 50 个 App 的大厅滚动与启动；切换前旧 Session 清理；系统手势、暂停恢复和
性能浮层；Texture retained 生命周期；Timer 积压；逻辑触摸坐标；亮度、主音量、音频完成和随机源；
电源安全点超时、重复按键、休眠唤醒与实际关机。具体时序和参数以对应实现与测试为准。

尚未完成的发布门槛：

1. Bundle requirements 与 WAMR instance 创建前的完整兼容性 preflight。
2. 权限声明、grant 与 method 级检查。
3. Resource/Graphics/Input 协议版本冻结、兼容 fixture 和 wire 负向/fuzz 回归。
4. Texture、Timer、Run/Stop 与五个集成应用的真机回归；Mosaico 传感器轴向和磁场校准验收。
5. 生产 package 签名与授权、网络配置、TLS 负向真机矩阵，以及在线安装/升级/卸载的断电和错误恢复矩阵。

## 9. 架构禁止项

不引入 Service Locator、深继承树或新的全局可变状态；不让组合根、ABI adapter 或 Platform 承担领域
Service 业务；不为新板型分叉 Guest API；不以裸 new/delete、无界容器或 detached task 管理实时资源。
第三方源码不因本项目的格式、命名或文档结构而改动。
