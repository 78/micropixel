# MicroPixel Firmware 源码导航

`main/` 只包含 MicroPixel 自维护的 ESP-IDF Host 代码。第三方 WAMR、LVGL 与 ESP-IDF component 不在此目录内。

## 依赖方向

```text
                         ┌─ owns/initializes ─ Platform ─ implements ─ Device/System UI contracts
FirmwareApp (组合根) ────┼─ creates ─ DeviceServices ── injects ─ AppRuntime
                         ├─ creates ─ SystemShell ─────── HostController
                         └─ creates ─ RemoteControlAgent ────────┘
                                                        │
                                               owns 0..1 AppSession
```

`device/` 和 `platform/` 在文件系统中同层，但职责不是平行重复：

- `device/` 定义与硬件无关的 Graphics、Input、Audio、Random、Battery、Devices、Sensors、GPIO、
  Haptics 能力契约和 Runtime 使用的 façade；
- `platform/` 实现这些契约，并持有开发板、驱动、LVGL 与外设生命周期；
- `runtime/` 只依赖 `device/`，不得 include `platform/`；
- `host_ui/` 定义 Host 原生 System Shell 与平台无关的 System UI model；
- `FirmwareApp` 是唯一知道具体 `Platform` 并把 backend 注入 `DeviceServices` 的组合根；`HostController` 管理
  Hall / Foreground 状态和 App 生命周期转移。

`conformance/` 是配置开启后才编译的 Host 合成事件钩子，不属于产品 Runtime。

## 完整文件列表

下面列出 `main/` 下全部自维护文件，不省略到目录级。`.cpp/.c` 是实现，配对的 `.hpp/.h` 是接口或内部契约。

```text
main/
├── README.md
├── CMakeLists.txt
├── Kconfig.projbuild
├── idf_component.yml
├── app_main.cpp
├── app_controller.cpp
├── app_controller.hpp
├── firmware_app.cpp
├── firmware_app.hpp
├── host_controller.cpp
├── host_controller.hpp
├── host_power_state.hpp
├── work/
│   ├── background_executor.cpp
│   └── background_executor.hpp
├── host_ui/
│   ├── system_shell.cpp
│   ├── system_shell.hpp
│   ├── system_gesture_router.cpp
│   ├── system_gesture_router.hpp
│   ├── system_settings_store.cpp
│   ├── system_settings_store.hpp
│   └── system_ui.hpp
├── remote_control/
│   ├── remote_control_agent.cpp
│   └── remote_control_agent.hpp
├── conformance/
│   ├── CMakeLists.txt
│   ├── guest_test_hooks.cpp
│   └── guest_test_hooks.hpp
├── device/
│   ├── CMakeLists.txt
│   ├── audio.hpp
│   ├── battery.hpp
│   ├── devices.hpp
│   ├── gpio.hpp
│   ├── graphics.hpp
│   ├── haptics.hpp
│   ├── input.hpp
│   ├── power.hpp
│   ├── random.hpp
│   ├── sensors.hpp
│   ├── device_services.cpp
│   └── device_services.hpp
├── platform/
│   ├── CMakeLists.txt
│   ├── platform.hpp
│   ├── audio_backend.hpp
│   ├── configured_backends.hpp
│   ├── random_backend.cpp
│   ├── graphics/
│   │   ├── command_stream.cpp
│   │   └── command_stream.hpp
│   ├── metalio-claw4/
│   │   ├── battery_backend.cpp
│   │   ├── battery_backend.hpp
│   │   ├── device_catalog.cpp
│   │   ├── device_catalog.hpp
│   │   ├── gpio_backend.cpp
│   │   ├── gpio_backend.hpp
│   │   ├── haptics_backend.cpp
│   │   ├── haptics_backend.hpp
│   │   ├── i2c_executor.cpp
│   │   ├── i2c_executor.hpp
│   │   ├── peripheral_ids.hpp
│   │   ├── platform.cpp
│   │   ├── sensor_backend.cpp
│   │   ├── sensor_backend.hpp
│   │   ├── graphics_adapter.cpp
│   │   ├── graphics_adapter.hpp
│   │   ├── system_ui_adapter.cpp
│   │   ├── system_ui_adapter.hpp
│   │   ├── board_hardware.cpp
│   │   ├── board_hardware.hpp
│   │   ├── icons/
│   │   │   ├── wifi_status_icons.c
│   │   │   └── wifi_status_icons.hpp
│   │   ├── audio/
│   │   │   └── synth_audio.cpp
│   │   ├── input/
│   │   │   ├── gt911_input.cpp
│   │   │   ├── gt911_input.hpp
│   │   │   ├── tca9555_power_key.cpp
│   │   │   └── tca9555_power_key.hpp
│   │   └── display/
│   │       ├── dirty_region_coalescer.cpp
│   │       ├── dirty_region_coalescer.hpp
│   │       ├── esp_lcd_nv3051f.c
│   │       ├── esp_lcd_nv3051f.h
│   │       ├── png_cover_decoder.cpp
│   │       ├── png_cover_decoder.hpp
│   │       ├── retained_scene.cpp
│   │       ├── retained_scene.hpp
│   │       ├── retained_surface.cpp
│   │       ├── retained_surface.hpp
│   │       ├── screen_capture.cpp
│   │       └── screen_capture.hpp
│   └── null/
│       ├── audio_backend.cpp
│       └── graphics_backend.cpp
└── runtime/
    ├── CMakeLists.txt
    ├── app_runtime.cpp
    ├── app_runtime.hpp
    ├── app_session.cpp
    ├── app_session.hpp
    ├── event_queue.cpp
    ├── event_queue.hpp
    ├── guest_context.cpp
    ├── guest_context.hpp
    ├── runtime_limits.hpp
    ├── touch_event_bridge.cpp
    ├── touch_event_bridge.hpp
    ├── key_event_bridge.cpp
    ├── key_event_bridge.hpp
    ├── abi/
    │   ├── abi_bridge.h
    │   ├── guest_abi.cpp
    │   ├── native_symbols.c
    │   ├── service_endpoints.cpp
    │   ├── service_endpoints.hpp
    │   ├── service_registry.cpp
    │   └── service_registry.hpp
    ├── bundle/
    │   ├── aot_package.cpp
    │   ├── aot_package.hpp
    │   ├── bundle_format.h
    │   ├── bundle_reader.c
    │   └── bundle_reader.h
    ├── bundlefs/
    │   ├── bundlefs.cpp
    │   ├── bundlefs.h
    │   └── bundlefs_format.h
    ├── resources/
    │   ├── bitmap_decoder.cpp
    │   ├── bitmap_decoder.hpp
    │   ├── bitmap_store.cpp
    │   ├── bitmap_store.hpp
    │   ├── resource_service.cpp
    │   └── resource_service.hpp
    ├── services/
    │   ├── gpio_service.cpp
    │   ├── gpio_service.hpp
    │   ├── haptics_service.cpp
    │   ├── haptics_service.hpp
    │   ├── sensor_service.cpp
    │   ├── sensor_service.hpp
    │   ├── service_result.hpp
    │   ├── storage_service.cpp
    │   ├── storage_service.hpp
    │   ├── timer_service.cpp
    │   └── timer_service.hpp
    └── wamr/
        ├── diagnostics.c
        ├── diagnostics.h
        ├── wamr_runtime.cpp
        ├── wamr_runtime.hpp
        ├── watchdog.c
        └── watchdog.h
```

## 子目录职责

- `runtime/abi/` 固定为 7 个文件。它包含 C ABI 声明、WAMR native symbol 表、参数适配、固定容量服务注册表和各服务 Endpoint；`ServiceHandler` 与注册表放在一起，避免为一个小抽象再增加文件。
- `runtime/bundle/` 负责 Bundle v1 解析、语义校验和 AOT payload 所有权。v1 使用显式长度的 64 字节
  AppId、必需的 UTF-8 App 标题元数据，并将 Header 固定为 128 字节；对外格式由 `bundle_format.h`
  固定，目录调整不改变磁盘 ABI。
- `runtime/bundlefs/` 是 24 MiB `app_store` 的底层文件系统。它以离散 64 KiB 数据块保存不可变 Bundle，
  使用四个 16 KiB Catalog Bank 环形提交，最多保存 50 个 App，并兼容读取和迁移旧 v1 的四个 4 KiB
  Bank。它提供不透明的 read/mmap/replace/remove 接口；Catalog 不使用 NVS，不扫描或安装预置 App；完整格式见
  [`docs/design/bundlefs.zh-CN.md`](../../../docs/design/bundlefs.zh-CN.md)。
- `work/` 提供一个固定容量、低优先级的后台执行器；App Hall 封面、Guest 压缩图片解码与 Wi-Fi NVS
  持久化共享它，避免为同类串行工作分别保留任务栈，也避免在 `sys_evt` 中执行 flash 写入。作业上下文由
  提交方持有，提交方必须等待完成或通过 shutdown protocol 证明其生命周期。
- `platform/metalio-claw4/i2c_executor.*` 为板上的单条物理 I²C bus 提供一个 4 KiB 固定容量优先级
  executor。Touch/电源、同步控制、Sensor/电池分别进入 high/normal/low 队列；ISR 和 timer callback 只投递
  作业。Sensor 与 GT911 不再各自保留任务栈，GPIO 和实时 I²S 不进入该 executor。
- `runtime/resources/` 负责资源请求、图片解码和 Bitmap handle/PSRAM 配额。Guest PNG 由 libpng
  逐行直接写入最终 ARGB8888 PSRAM buffer，避免整图 inflate 临时副本和第二遍整图颜色转换干扰显示
  framebuffer scanout；压缩图片解码提交到共享后台执行器，Guest 调用仍同步等待结果。
- `runtime/services/` 放 Runtime 自己提供的 Timer、Storage，以及为 Sensors、GPIO、Haptics 管理
  Guest-local handle、事件与 Session 生命周期的业务；它们不是物理设备 backend。
- `runtime/wamr/` 负责 WAMR 初始化、module/instance/exec-env RAII、watchdog 和运行期诊断。Guest watchdog
  使用单调时钟 deadline 和共享的 ESP Timer task，不为每次 WAMR 调用创建 pthread。
- `runtime/app_runtime.*` 持有长驻 WAMR，并同步创建最多一个 `AppSession`；`runtime/app_session.*` 持有一次
  Guest 的 Bundle、module、instance、exec-env 与 `GuestContext` 销毁边界。
- `host_ui/` 是 Host 原生 App Hall/状态层的控制边界；具体绘制仍由所选 Platform 的 `SystemUiBackend` 完成，FPS 开关、亮度与音量保存到独立的 `sys_store` NVS。
- `remote_control/` 是 Host 拥有的远程调试 Agent：独立任务维护 HTTP/3 控制流和有界命令队列，设备
  UUID/credential 与 Remote Control 开关分别保存在 `sys_store/control`；它只向 System Shell 发布快照，
  不从网络回调直接调用 WAMR、LVGL 或板级驱动。发布配置必须提供匹配 Control 主机名的 DER CA
  base64 和可达 NTP 服务；设备先建立可信 wall clock，再校验证书链/用途/有效期/主机名、TLS 1.3
  CertificateVerify 与 Finished。开发 bypass 只跳过证书链、有效期和主机名。
- `app_controller.*` 在单独 pthread 上运行唯一 Guest，并把 `NotRunning / Starting / Foreground / Suspending /
  Suspended / Resuming / Stopping` 内部状态收敛为同步 Host 命令；Guest SDK 不暴露暂时无用的 lifecycle callback。
- Resume/Stop 通过两个 typed Core Event 暴露给 Guest；正常切换先协作 Stop，500ms 超时才强制 terminate，
  真实 trap 的 WAMR 调用栈诊断保持开启。
- 单击电源键由 Host 的 `Awake → EnteringSleep → Asleep → Waking → Awake` 状态机统一编排，不先返回大厅，
  也不新增 `Pause` 事件。前台 App 先用现有 `Suspend()` 控制消息等待 Guest 停在 `event_wait` 安全点，再渐暗
  背光、暂停 LVGL、释放 LCD/DSI 并进入 light sleep；唤醒后重建并重绑显示，再用现有 `Resume()` 让同一
  Session 首先收到 `Resume`。500ms 内未到安全点时走现有强制 Stop，唤醒后显示大厅；OTA 正在写入时忽略
  电源键。进入低功耗的过渡期若再次检测到物理按下，则取消本次入睡并恢复显示；非 `Awake` 状态拒绝新的
  入睡请求。按键唤醒后必须先确认物理释放才重新开放电源请求，同时用 click guard 防止快速连按或释放事件
  在显示恢复后立即触发再次休眠。平台在真正入睡前排空 TCA9555 共享中断并确认中断线释放；ESP-IDF 因
  瞬态唤醒条件返回 `ESP_ERR_SLEEP_REJECT` 时保持显示关闭并原地重试，不执行一次伪唤醒恢复。
- System Settings 的 Power Management 页面使用 LVGL switch/dropdown 配置自动休眠（Off 或 1/5/10/30
  分钟，默认 5 分钟）。Host 只在外接电源状态明确为未连接时累计无交互时间；触摸、系统手势和远控注入输入
  都会刷新 deadline，插电暂停，拔电和唤醒重新计时。到期请求复用上述短按电源键状态机，不新增另一条平台
  sleep 路径；旧 `sys_store/system` v1 设置记录兼容迁移到包含自动休眠字段的 v2。
- 亮屏且处于 `Awake` 时，新一轮电源键长按满 2 秒进入终态 `ShuttingDown`；唤醒所用且尚未释放的同一轮
  长按始终被拒绝。Host 停止当前 App、取消远控输入并静音后显示 `Shutting down...`，然后立即由
  Metalio-Claw4 backend 按板级 `PWR_KEY_PULSE` 协议持续驱动 TCA9555 P0.4（高/低各 100ms）。关机画面
  在脉冲执行期间保持可见，不额外固定等待，直到电源管理芯片切断电源。OTA 写入期间拒绝该请求。
- App Hall 最多展示 50 个 App，首行三张完整卡片并露出下一张，支持触控左右自由拖动和基于释放速度的
  惯性滚动，不强制按卡片位置吸附。卡片作为一个裁剪内容带移动，每帧只 invalidates 大厅视口；面板
  partial-buffer 通过 DMA2D 传输，较大的 LVGL fill/blend 使用 PPA。Catalog 变化时回到最左侧，使新安装在
  index 0 的 App 立即可见。大厅只保留与视口相交项及左右各一个预取项，最多 6 套卡片控件和 6 张
  202x202 RGB888 解码封面；快速滑动时先显示占位图，共享后台执行器只为当前窗口解码，并丢弃已经滑过的结果。
  压缩 PNG 保持 Flash mmap，不复制到 PSRAM。唯一挂起 App 的卡片改用窗口截图，并由
  ESP32-P4 PPA 完成 720x720 ↔ 卡片区域的硬件缩放动画；全尺寸截图只在切换期间存在，不做软件缩放
  fallback。过渡合成器分别维护无运行卡片的静态大厅 baseline 和当前动画工作背景；从挂起 App 切换到
  另一个 App 时复用静态 baseline，不在启动关键路径重新缓存带 `RUNNING` 卡片的大厅。大厅顶部由 Host
  显示基于 LVGL 内置 Wi-Fi 字形生成的 RSSI 分级图标和 LVGL 多档电池图标；
  USB 或无线外部电源接入时，电池图标立即切换为绿色充电符号，断开后恢复当前电量档位；
  蜂窝信号使用 Host 绘制的分级信号柱，真实蜂窝后端可用前不显示。状态层使用 LVGL primitive 和单次半透明合成；亮度、主音量及居中的 FPS/聚合 CPU 小蒙层
  均由 Host 控制。
- Wi-Fi 冷启动和掉线发现使用被动扫描，只按已保存网络的信道去重后逐个检查，并优先检查上次连接网络的
  信道；发现候选后才连接。已连接链路掉线后只做一次快速重连，失败后立即进入被动发现，未发现候选时按
  1、2、5、15 分钟退避。Wi-Fi 首页只管理开关和已保存网络；“Connect to New Wi-Fi”子页才执行主动全信道
  扫描，而用户在已保存网络菜单中明确选择“Connect”时也不锁定旧信道、BSSID 或频段。每轮 `SCAN_DONE`
  后等待 10 秒再开始下一轮，已连接时使用驱动的 background scan。后端状态变化
  通过非阻塞、可合并的 Host 事件主动更新 App Hall、系统菜单、Status Layer 和 Wi-Fi 页面；只有扫描间隔
  与性能采样等真正的周期任务使用定时等待。
- `platform/graphics/` 是跨板级图形协议校验；`platform/metalio-claw4/` 只放该开发板的实现，并按真实硬件子系统分为 `display/`、`input/`、`audio/`；Battery backend 复用板级 I²C 总线读取 BQ27220 电量和充电电流，并通过 TCA9555 的 USB / 无线充电检测输入及共享中断报告外部电源状态。
- `platform/null/` 提供没有真实板级设备时的构建实现。

只在 `device/`、`runtime/`、`platform/`、`conformance/` 这些已有子系统设置独立 CMake 清单；当前很小的
`host_ui/` 由顶层清单直接收录，不为 `abi/` 等叶子目录继续增加小型 CMake 文件。
