# MicroPixel Firmware 源码导航

`main/` 是 MicroPixel 自维护的 ESP-IDF Host 代码。本文帮助定位修改入口；设计理由见
[架构文档](../../../docs/design/architecture.zh-CN.md)，构建见
[烧录指南](../../../docs/development/flashing.zh-CN.md)。

## 依赖方向

```text
FirmwareApp：装配 Platform、DeviceServices、AppRuntime 和 SystemShell
                          Runtime → Device contracts ← Platform
```

Runtime 通过注入的契约使用硬件，Platform 只登记初始化成功的能力。Host Controller 编排应用，
System UI 管理页面和交互，Board 负责物理呈现。新增板型不应要求修改 Guest ABI 或应用生命周期。

## 按任务定位

| 要修改的行为 | 入口 | 职责边界 |
|---|---|---|
| 启动与装配 | [firmware_app.cpp](firmware_app.cpp) | 唯一同时知道 Platform、Device 和 Runtime 的组合根 |
| 硬件无关能力 | [device/contracts](device/contracts/) | 契约与能力描述，不泄漏板型类型 |
| 设备发现 | [device](device/) | 设备身份、目录、共享校验与 Runtime façade |
| 应用启动、切换、暂停 | [host/controller](host/controller/) | 单 App 生命周期和有界控制命令 |
| 本地与远程控制 | [host/controller/local](host/controller/local/)、[remote](host/controller/remote/) | 复用 Dispatcher，不从网络回调直接调用 WAMR/LVGL |
| 大厅、状态层和系统手势 | [host/ui](host/ui/) | 页面状态、布局 profile、封面窗口与交互 |
| 电源编排 | [host/controller](host/controller/)、[host/ui/system_shell.cpp](host/ui/system_shell.cpp) | 协调应用安全点与平台休眠/关机 |
| 系统时钟与日志 | [host/time](host/time/)、[host/logging](host/logging/) | Host 时间可信度与有界日志 |
| Session 与 WAMR | [runtime](runtime/)、[runtime/wamr](runtime/wamr/) | Bundle/module/instance/执行环境的所有权与退出清理 |
| ABI 接入 | [runtime/abi](runtime/abi/) | Guest 内存验证、wire 转换和 Endpoint 转发 |
| Service 业务 | [runtime/services](runtime/services/) | Guest-local handle、容量、事件和 Session 生命周期 |
| 音频播放与 PCM 推流 | [runtime/audio](runtime/audio/) | 解码、环形缓冲与完成/low-water 事件 |
| 图片和资源 | [runtime/resources](runtime/resources/) | 加载、解码与资源引用生命周期 |
| Host 光栅算法 | [runtime/graphics](runtime/graphics/) | 可单测的绘制记录校验与像素 kernel |
| Bundle 格式与安装策略 | [runtime/bundle](runtime/bundle/) | 容器解析、AOT target 和 App 语义 |
| App Store 持久化 | [runtime/bundlefs](runtime/bundlefs/) | 数据块、Catalog、写时复制和恢复 |
| 后台工作与任务策略 | [work](work/) | 固定容量执行器、任务核心与优先级 |

## Platform 内部怎么找

| 目录 | 负责什么 |
|---|---|
| [boards](platform/boards/) | 引脚、供电、启动顺序、板载外设与能力注册 |
| [targets](platform/targets/) | 选择 SoC 组件依赖 |
| [drivers](platform/drivers/) | 按器件型号复用的驱动，不依赖 Board |
| [buses](platform/buses/) | 共享物理总线的串行调度 |
| [audio](platform/audio/)、[haptics](platform/haptics/)、[wifi](platform/wifi/) | 跨板领域引擎与外设实现 |
| [adapters](platform/adapters/) | 具体能力到 Device 契约的窄适配 |
| [graphics](platform/graphics/) | 像素合成、PPA/DMA2D 与共享图形支持 |
| [lvgl](platform/lvgl/) | Guest 图形、显示发布、扫描输出与 LVGL 桥接 |
| [transports](platform/transports/) | 本地控制字节传输 |
| [defaults](platform/defaults/) | unavailable 能力实现 |

板级显示管线持有 panel、transport 和 framebuffer。系统页面位于 `host/ui/`，板级只选择完整的布局
profile 并提供转场、截图、亮度等能力。不要在驱动中引入板名分支，也不要在板目录复制公共 UI。
术语约定见 [Firmware 分层与命名](../../../docs/design/firmware-terminology.zh-CN.md)。

## 修改前应确认的边界

- 异步工作先确认上下文所有权、停止接收和 join 顺序；任务策略从
  [task_policy.hpp](work/task_policy.hpp)取得，ISR 不直接执行 Guest 或 LVGL 工作。
- Scene、DirectSurface、显示管线之间先确认谁可写 buffer、谁持有在飞帧，以及系统 UI 接管时的同步。
  图形链路见 [性能诊断](../../../docs/development/graphics-performance.zh-CN.md)。
- 修改 Bundle 时区分容器语义和文件系统事务；格式与掉电规则见
  [BundleFS](../../../docs/design/bundlefs.zh-CN.md)。
- 新板型添加 Board、Kconfig choice、CMake 选择与
  [构建 profile](../../../tools/firmware_profiles.json)，复用设备契约和控制流程。
  Null profile 只验证依赖方向，不提供真机语义。
- 传感器和 GPIO 只在被使用时启用相应采样或事件工作；板级未开放的引脚不进入设备目录。

## 验证入口

Host 单元测试只通过 `bash tools/tests/test_firmware_host.sh` 编译运行。Firmware 修改还需格式检查和
P4 Host 构建；共享图形、LVGL 或 PPA 条件分支修改需同时构建 S31 与至少一款 S3。
System Shell 和其他 Firmware 修改使用 `bash tools/p4.sh build-host`，不构建 Guest 或 App Store。
Bundle 或集成 App 修改按范围运行相关测试并构建对应正式 Bundle。
完整门禁见 [CONTRIBUTING](../../../CONTRIBUTING.md)，S3 特有约束见
[S3 适配指南](../../../docs/development/esp32-s3-box-3-bring-up.zh-CN.md)。

App Hall 中短按卡片打开 App，长按卡片直接显示与系统菜单 App Management 共用的操作面板
（打开、卸载、取消）。关闭面板或完成卸载后返回大厅；卸载仍需确认，且运行中的 App
必须先从大厅停止，才能卸载。

Wi-Fi 已保存网络、App Management、大厅长按及远程控制确认共用底部 action sheet，
打开时以 100 ms 减速动画从屏幕下方滑入；遮罩立即拦截背景点击，关闭仍立即生效。
动画每一步主动请求显示刷新，避免静态场景的低频刷新定时器跳过中间帧。

App Hall 封面仅顶部保留圆角，底部以直角衔接标题区；启动界面复用封面时，
底部保留原图像素，避免带入卡片底色。整张卡片仍保留外框圆角。

App 操作面板顶部左侧显示 App 名称，右侧显示 Bundle 占用大小。App Management 列表在名称下方
同一行显示 App ID 和占用大小；长名称与 App ID 以省略号截断，保留大小可见。

App Management 的固定容量模型由菜单调用持有的 RAII 对象优先分配在 PSRAM，退出菜单时释放；
首次进入和卸载后原地填充、复用同一份存储。不要在菜单循环中
按值返回再赋值整个模型：50 个 App 的临时副本会长期占用调用者栈帧，叠加 System Settings
调用层和控制命令后可能耗尽主任务栈。回归需覆盖菜单进入、卸载确认/取消、确认卸载后的列表刷新，
并通过 `micropixel device diagnostics` 检查 `main` 的栈余量。

Remote Control 保存生成接口返回的配对 ID。收到当前控制会话的 `pairing.consumed` 帧时，
仅清除匹配 ID 的连接码与倒计时，并立即恢复生成入口。重复通知或旧码通知不会影响新码。
控制流断开时清除本地连接码，重连后可重新生成；服务端在新生成时替换旧码，避免漏收通知造成等待。
