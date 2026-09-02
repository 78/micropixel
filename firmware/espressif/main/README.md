# MicroPixel Firmware 源码导航

`main/` 只包含 MicroPixel 自维护的 ESP-IDF Host 代码。第三方 WAMR、LVGL 与 ESP-IDF component 不在此目录内。

## 依赖方向

```text
                         ┌─ owns/initializes ─ Platform ─ implements ─ Device/System UI contracts
FirmwareApp (组合根) ────┼─ creates ─ DeviceServices ── injects ─ AppRuntime
                         ├─ creates ─ SystemShell ─────── HostController
                         └─ creates ─ ControlDispatcher ─ Local / Remote Agent
                                                        │
                                               owns 0..1 AppSession
```

`device/` 和 `platform/` 在文件系统中同层，但职责不是平行重复：

> Board 初始化 Driver，把可用 Peripheral、Controller 和 Presentation 登记给 Platform；Platform 将
> Peripheral 转成公开 Device，由 Service 管理，并通过 ABI Endpoint 提供给 Guest。

- `device/contracts/` 定义与硬件无关的 Graphics、Input、Audio、Random、Battery、Devices、Sensors、GPIO、
  Haptics 能力契约，`device/` 根部保留 Runtime 使用的 façade 和共享格式/校验；
- `platform/` 实现这些契约，并持有开发板、驱动、LVGL 与外设生命周期；
- `runtime/` 只依赖 `device/`，不得 include `platform/`；
- `host/ui/` 定义 Host 原生 System Shell 与平台无关的 System UI model；
- `FirmwareApp` 是唯一读取 `PlatformServices` 并注入 `DeviceServices` 的组合根；`HostController` 管理
  Hall / Foreground 状态和 App 生命周期转移。

`conformance/` 是配置开启后才编译的 Host 合成事件钩子，不属于产品 Runtime。

## 目录与关键文件

下面列出 `main/` 的稳定分层和关键入口；叶子实现使用 `*` 合并展示。`.cpp/.c` 是实现，配对的
`.hpp/.h` 是接口或内部契约。

```text
main/
├── README.md
├── CMakeLists.txt
├── Kconfig.projbuild
├── idf_component.yml
├── app_main.cpp
├── firmware_app.cpp
├── firmware_app.hpp
├── host/
│   ├── CMakeLists.txt
│   ├── logging/
│   │   └── system_log_buffer.*    # Host/App 统一有界日志总线与 ESP_LOG 接管
│   ├── controller/
│   │   ├── CMakeLists.txt
│   │   ├── app_controller.*
│   │   ├── control_{types,dispatcher}.*
│   │   ├── host_controller.*
│   │   ├── host_power_{state,coordinator}.*
│   │   ├── local/
│   │   │   └── local_control_agent.*
│   │   └── remote/
│   │       ├── remote_control_{agent,protocol}.*
│   │       ├── remote_identity_store.*
│   │       └── remote_reconnect_policy.hpp
│   ├── time/
│   │   ├── network_time.*
│   │   └── system_time.*
│   └── ui/
│       ├── CMakeLists.txt
│       ├── i18n/
│       ├── system_shell.*
│       ├── system_gesture_router.*
│       ├── system_locale.*
│       ├── system_settings_store.*
│       ├── system_ui.hpp
│       └── lvgl/square_common/   # Host 拥有的共享 App Hall、状态层与系统页面
├── work/
│   ├── CMakeLists.txt
│   ├── background_executor.cpp
│   ├── background_executor.hpp
│   └── task_policy.hpp
├── conformance/
│   ├── CMakeLists.txt
│   ├── guest_test_hooks.cpp
│   └── guest_test_hooks.hpp
├── device/
│   ├── CMakeLists.txt
│   ├── contracts/
│   │   ├── audio.hpp
│   │   ├── battery.hpp
│   │   ├── devices.hpp
│   │   ├── gpio.hpp
│   │   ├── graphics.hpp
│   │   ├── board_info.hpp
│   │   ├── haptics.hpp
│   │   ├── input.hpp
│   │   ├── local_control.hpp
│   │   ├── power.hpp
│   │   ├── random.hpp
│   │   ├── sensors.hpp
│   │   └── wifi.hpp
│   ├── device_registry.*          # 上层设备登记、opaque ID 分配与 Peripheral 路由
│   ├── device_services.cpp
│   ├── device_services.hpp
│   ├── font_cbin_format.h
│   └── text.*
├── platform/
│   ├── CMakeLists.txt
│   ├── platform.*                 # Board + BoardRegistration + PlatformServices
│   ├── adapters/                  # 具体操作到 Device contract 的窄适配
│   │   ├── CMakeLists.txt
│   │   └── graphics_adapter.*
│   ├── audio/                     # 公共 AudioEngine、Mixer 与输出 Peripheral 接口
│   ├── haptics/                   # 公共定时 Haptics Peripheral
│   ├── wifi/                      # Wi-Fi Manager 与 Radio
│   ├── random/                    # 系统随机实现
│   ├── defaults/                  # 未提供能力的默认实现
│   ├── controllers/               # 可复用的板级控制算法
│   ├── buses/                     # 共享物理总线调度
│   │   ├── CMakeLists.txt
│   │   └── i2c_executor.*
│   ├── graphics/
│   │   ├── guest_scene.cpp
│   │   └── app_surface_compositor.cpp
│   ├── drivers/                   # 按器件组织，不知道具体开发板
│   │   ├── display/nv3051f/esp_lcd_nv3051f.[ch]
│   │   ├── power/bq27220.*
│   │   └── sensors/{sc7a20htr,qmc6309,bmi270,bmm150,vector_sensor}.*
│   ├── input/                     # 厂商触摸句柄到 Device Input 的共享实现
│   │   ├── esp_lcd_touch_input.*
│   │   └── gt911_input.*
│   ├── lvgl/                      # LVGL 平台桥：显示、字体、Guest renderer 与输入路由
│   │   ├── CMakeLists.txt
│   │   ├── display/
│   │   ├── fonts/
│   │   ├── guest_graphics_engine.*
│   │   ├── guest_graphics_operations.* # Graphics contract 的共享 forwarding
│   │   └── host_pointer_router.hpp      # Host UI 固定容量指针队列与 LVGL indev
│   ├── transports/                # 本地控制字节传输与开发显示命令
│   ├── targets/{esp32p4,esp32s31}/ # SoC 依赖 profile
│   └── boards/                    # 唯一允许组合具体板级引脚/外设的位置
│       ├── metalio-claw4/
│       │   ├── CMakeLists.txt
│       │   ├── platform.cpp
│       │   ├── platform_state.hpp
│       │   ├── board_config.hpp
│       │   ├── board_io.*
│       │   ├── presentation.*
│       │   ├── {battery,gpio,sensor}_peripheral.* + haptic_actuator.*
│       │   ├── i2s_audio_sink.*
│       │   ├── display/screen_capture.*
│       │   └── tca9555_power_key.*
│       ├── esp-mosaico/
│       │   ├── CMakeLists.txt
│       │   ├── platform.cpp
│       │   ├── platform_state.hpp
│       │   ├── board_config.hpp
│       │   ├── presentation.*
│       │   ├── i2s_audio_sink.*
│       │   ├── display/
│       │   ├── {battery,sensor}_peripheral.* + power_controller.*
│       │   ├── function_button.* + status_led.* + haptic_actuator.*
│       │   └── usb_cdc_console.*
│       └── null/
│           ├── CMakeLists.txt
│           └── platform.cpp
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
  固定，目录调整不改变磁盘 ABI。每个 AOT section 在 `reserved0` 以 bitmap 声明 CPU target；当前只装载一个 AOT，
  安装事务在开始 BundleFS staging write 前拒绝缺少 target 元数据或与 Host 架构不匹配的 App Bundle。
  该 per-section 布局为未来多 target Bundle 保留扩展空间，但当前 reader 尚未启用多 AOT 选择。
- `runtime/bundlefs/` 是 `app_store` 的底层文件系统（P4 产品为 24 MiB，S31 NOR bring-up profile 为
  8 MiB）。它以离散 64 KiB 数据块保存不可变 Bundle，并按目标将每块展开为一个或多个 Flash MMU page，
  使用四个 16 KiB Catalog Bank 环形提交，最多保存 50 个 App，并兼容读取和迁移旧 v1 的四个 4 KiB
  Bank。它提供不透明的 read/mmap/replace/remove 接口；Catalog 不使用 NVS，不扫描或安装预置 App；完整格式见
  [`docs/design/bundlefs.zh-CN.md`](../../../docs/design/bundlefs.zh-CN.md)。
- `work/` 提供一个固定容量、低优先级的后台执行器；App Hall 封面、Guest 压缩图片解码与 Wi-Fi NVS
  持久化共享它，避免为同类串行工作分别保留任务栈，也避免在 `sys_evt` 中执行 flash 写入。作业上下文由
  提交方持有，提交方必须等待完成或通过 shutdown protocol 证明其生命周期。
- `platform/buses/i2c_executor.*` 为板上的单条物理 I²C bus 提供一个 4 KiB 固定容量优先级
  executor。Touch/电源、同步控制、Sensor/电池分别进入 high/normal/low 队列；ISR 和 timer callback 只投递
  作业。Sensor 与 GT911 不再各自保留任务栈，GPIO 和实时 I²S 不进入该 executor。
- `runtime/resources/` 负责资源请求、图片解码和 Bitmap handle/PSRAM 配额。Guest PNG 由 libpng
  逐行直接写入最终 ARGB8888 PSRAM buffer，避免整图 inflate 临时副本和第二遍整图颜色转换干扰显示
  framebuffer scanout；压缩图片解码提交到共享后台执行器，Guest 调用仍同步等待结果。
- `runtime/services/` 放 Runtime 自己提供的 Timer、Storage，以及为 Sensors、GPIO、Haptics 管理
  Guest-local handle、事件与 Session 生命周期的业务；它们不是物理 Peripheral。
- `runtime/wamr/` 负责 WAMR 初始化、module/instance/exec-env RAII、watchdog 和运行期诊断。Guest watchdog
  使用单调时钟 deadline 和共享的 ESP Timer task，不为每次 WAMR 调用创建 pthread。
- `runtime/app_runtime.*` 持有长驻 WAMR，并同步创建最多一个 `AppSession`；`runtime/app_session.*` 持有一次
  Guest 的 Bundle、module、instance、exec-env 与 `GuestContext` 销毁边界。
- `host/ui/` 是 Host 原生 App Hall/状态层的呈现边界；`SquareSystemUiState` 统一拥有方屏 UI 对象、状态和
  主题、启动/加载页、Guest 前台层级和页面生命周期，`virtualized_hall_policy.*` 在 Host UI 层以有界窗口
  物化卡片和解码封面；480/720 profile 各自完整定义对应分辨率的产品 UI 属性。所选 Board 的
  `SquarePresentation` 只接入显示转场、截屏与刷新机制，并消费 Host 算好的 Hall 转场区域，不再读取
  Hall/UI 内部状态。
  FPS 开关、亮度与
  音量保存到独立的 `sys_store` NVS。Host UI 不
  依赖 `host/controller/`。
- `host/time/` 负责 Host 全局 SNTP 初始化、可信 UTC 判断和系统状态栏时钟格式，不进入 Guest ABI。
- `host/ui/lvgl/square_common/hall_scene_ui.*` 是 App Hall 的唯一页面实现，固定 P4 的文案、视觉层级、
  顶部状态栏、Settings/Update 按钮、轮播容器和错误状态；`profiles/square_720.hpp` 与
  `profiles/square_480.hpp` 提供完整的分辨率 UI profile。板级 `platform.cpp` 只选择 profile 并接入硬件转场，
  不再接管封面缓存、卡片窗口、事件或 Hall 生命周期。
- `host/ui/lvgl/square_common/status_layer_ui.*` 是 Claw4 与 Mosaico 唯一共用的顶部状态层；以 Mosaico
  的快捷卡片和双滑杆交互为基准，Wi-Fi/FPS 使用 `lv_button`，亮度/音量使用 `lv_slider`，遮罩点击与上滑
  关闭由 LVGL event 处理，Storage/Memory/SRAM 指标使用 `lv_bar`。板级统一通过 `HostPointerRouter` 接入，
  只保留显示转场、亮度和音量能力调用。
- `host/ui/lvgl/square_common/guest_gesture_hint_ui.*` 是前台 Guest 的底部系统手势提示；启动或恢复 App 时
  显示 3 秒后自动隐藏。底部上滑仅从屏幕中央三分之一区域开始识别，左右两侧继续传给 Guest，便于 App
  在底部布置按钮。提示条是复用的 Host LVGL 对象，只在显隐时请求局部刷新，不进入 Guest 的逐帧提交。
- `host/controller/` 集中 Host Controller 以及本地、远程控制入口；`control::ControlDispatcher` 是
  Local/Remote 共用的传输无关有界队列和快照边界，两类 Agent 不互相依赖。远程 Agent 以独立任务维护
  HTTP/3 控制流，设备
  UUID/credential 与 Remote Control 开关分别保存在 `sys_store/control`；配置服务地址且没有已保存开关时，
  Remote Control 默认开启并等待 Wi-Fi，用户保存的开关选择在后续启动时优先；它只向 System Shell 发布快照，
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
  Metalio-Claw4 `PowerController` 按板级 `PWR_KEY_PULSE` 协议持续驱动 TCA9555 P0.4（高/低各 100ms）。关机画面
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
  BQ27220 确认存在充电电流且尚未报告 Full Charged 时，电池图标切换为绿色充电符号；满电或充电停止后
  恢复当前电量档位。USB 或无线外部电源连接状态独立用于自动休眠策略，不再替代充电状态；
  蜂窝信号使用 Host 绘制的分级信号柱，真实蜂窝能力可用前不显示。状态层使用 LVGL primitive 和单次
  半透明合成；亮度、主音量及居中的 Guest 实际呈现 FPS/聚合 CPU 小蒙层均由 Host 控制。性能蒙层只在
  Guest App 前台运行时显示；App Hall、系统菜单、Status Layer、启动页和挂起状态均隐藏。FPS 只在包含
  Guest 内容的刷新完成后计数，不包含蒙层自身或其他 Host UI 的局部刷新。
- Wi-Fi 冷启动和掉线发现使用被动扫描，只按已保存网络的信道去重后逐个检查，并优先检查上次连接网络的
  信道；发现候选后才连接。已连接链路掉线后只做一次快速重连，失败后立即进入被动发现，未发现候选时按
  1、2、5、15 分钟退避。Wi-Fi 首页只管理开关和已保存网络；“Connect to New Wi-Fi”子页才执行主动全信道
  扫描，而用户在已保存网络菜单中明确选择“Connect”时也不锁定旧信道、BSSID 或频段。每轮 `SCAN_DONE`
  后等待 10 秒再开始下一轮，已连接时使用驱动的 background scan。Wi-Fi 状态变化
  通过非阻塞、可合并的 Host 事件主动更新 App Hall、系统菜单、Status Layer 和 Wi-Fi 页面；只有扫描间隔
  与性能采样等真正的周期任务使用定时等待。
- `platform/graphics/` 是跨板级图形协议校验；`platform/boards/metalio-claw4/` 只放该开发板的实现，并按真实硬件子系统分为 `display/`、`input/`、`audio/`；`BatteryPeripheral` 复用板级 I²C 总线读取 BQ27220 电量和充电电流，并通过 TCA9555 的 USB / 无线充电检测输入及共享中断报告外部电源状态。
- `platform/boards/null/` 提供没有真实板级设备时的构建实现。
- `platform/audio/` 在 App 启动或 Resume 后立即打开音频输出，并在 App 保持前台时持续输出静音帧以保持
  I²S、codec 和功放就绪；只有 App Suspend、Stop 或 Session 销毁后，原有 10 秒静音空闲计时才允许关闭输出。
- `platform/boards/esp32-s3-box-3/` 是 ESP32-S3 preview 组合：320×240 RGB565 SPI panel、displayed shadow
  和所有大图形/截图缓冲固定使用 PSRAM；原生 Wi-Fi 复用共享 `WifiManager`，ES8311/I2S 复用
  `platform/audio/` 的可配置 sink。GPIO1 侧边 Mute 由 Host 独占并归零 mixer，不作为 Guest Key，也不改变
  保存的设备主音量。板载 ICM-42607-P 通过共享 I2C executor 发布加速度和角速度；Pmod 排除 USB 的
  GPIO19/20 与 Dock I2C 的 GPIO40/41 后发布其余 12 路 GPIO。AHT30 位于外接 SENSOR 扩展板，不作为主机
  内建设备注册。板级音频、Wi-Fi 或传感器初始化失败只降级对应能力，不阻断 Hall、USB local control 或 Runtime。
- `platform/boards/m5stack-cores3/` 复用同一套 320×240 S3 UI、Guest Graphics、原生 Wi-Fi、USB local
  control、I²C executor 和 BMI270 Peripheral，板级只组合 ILI9342C、FT6336U、AXP2101/AW9523 供电与
  AW88298 单声道音频。`platform/audio/I2sCodecAudioSink` 统一拥有 I²S、DMA、采样转换和
  `esp_codec_dev` 生命周期，ES8311 与 AW88298 各自只实现 codec 创建配置。
- `platform/boards/esp-mosaico/` 是 ESP32-S31 的 P0/P1 产品组合：复用原生 Wi-Fi policy、共享 App Hall/
  Status Layer、480 方屏 layout、逻辑坐标变换、PPA/DMA2D 图形原语和 16 KiB MMU page-safe BundleFS；板级层
  组合 CO5300、`78/esp_lcd_touch_cst92xx`、ES8311 codec、BQ27220、数字振动电机、供电和引脚；音频 tone/PCM mixer 与
  Host 对数主音量曲线复用 `platform/audio/`，codec 控制与 Touch/Battery 共用 I²C executor。BQ27220 在同一
  executor 上应用官方 80 mAh fixed-EDV profile，并读取 SOC、电流和 Full Charged 状态；大厅只在确认仍有
  充电电流且未满电时显示充电符号，外部供电连接状态继续独立用于自动休眠策略。P4/S31
  的转场使用同一时间线与 PPA SRM 封装；S31 在
  原生 RGB565 中完成缩放/合成，再以独立的 1:1 PPA pass 生成 CO5300 线序，普通 LVGL flush 也使用相同
  的硬件打包原则，正常帧路径不做 CPU 整图逐像素颜色转换。BMI270 加速度/陀螺仪与两颗 BMM150 使用
  固定版本的 Bosch SensorAPI，由板级共享 I²C executor 串行执行配置和采样；GPIO57 监听实体 POWER 键并
  作为 light-sleep 唤醒源，低电平开漏仍承担 SAM8108 关机请求。Function Button 作为逻辑 Confirm key
  向 Guest 发布 down/up；GPIO3 橙色单色状态 LED 以逻辑归一化、只输出的 GPIO Device 开放，Guest 写
  `true` 即点亮，不感知物理低有效。17 根安全扩展 GPIO 和两秒主动电池刷新也已接入。CoreBoard V1.0 未把 HUSB320/TP4057 状态脚接回 MCU，因此
  外部供电由 BQ27220 的充放电方向推断；传感器轴向仍需真机姿态验收。NAND 与模块发现保留到 P2。
- 跨板实现按领域放入 `platform/audio/`、`platform/haptics/`、`platform/wifi/` 等目录；
  `platform/adapters/` 放窄接口适配器，`platform/buses/` 负责共享总线调度；`platform/drivers/` 按芯片
  型号组织，不能 include `boards/`；`platform/lvgl/` 按显示能力组织，不能写 Metalio-Claw4 引脚或电源时序；
  方屏 UI state/profile 位于 `host/ui/lvgl/`。`platform/boards/<board>/` 是组合层，选择共用实现并拥有剩余的
  板级 wiring。
- 两块产品板共用上层 `DeviceRegistry`、`BoardInfo` 和定时 Haptics Peripheral；板目录只登记实际存在的
  Peripheral、填写板型元数据并提供电机 actuator，公开 Device ID 由 Platform 分配。Claw4 与 Mosaico 的引脚、总线、地址和
  LEDC/UART/I²S 资源分配分别集中在各自的 `board_config.hpp`，驱动实现不再散落裸 GPIO 常量。
- Claw4 与 Mosaico 共用 `GuestGraphicsOperations`、`HostPointerRouter` 和 `SquareSystemUiState`：Graphics
  contract forwarding、Host UI 指针排队、LVGL indev、启动/Launch 页面、Hall bookkeeping、系统页面及
  Status Layer 生命周期保持同一语义。RGB888/MIPI-DSI 与 RGB565/QSPI 的像素所有权、flush 和硬件合成仍
  留在各自显示边界。
- 新增板型时，新增 `boards/<board>/CMakeLists.txt` 与实现，在 `MICROPIXEL_BOARD` choice 注册一个 symbol，
  再由 `platform/CMakeLists.txt` 选择它，并在 `tools/firmware_profiles.json` 增加声明式构建 profile；板型
  shell 只保留产品流程别名，ESP-IDF build/flash/monitor 和端口芯片核验统一委托给 `tools/firmware.py`。
  不得复制 Guest ABI、Runtime Service、共用器件驱动或现有 LVGL profile。`device::BoardInfo` 是系统信息与
  远控板型描述的唯一来源，不再在 HostController 中写死当前板名。
- `bash tools/p4.sh build-null` 使用独立 build 目录编译 Null board，是 Platform/Runtime 反向依赖门禁；该
  镜像没有真实硬件语义，脚本不会提供 flash 路径。
- `bash tools/s31.sh build-null` 是不可烧录的 S31 依赖方向门禁；`build-host`、`flash-host` 和
  `monitor` 面向物理 ESP-Mosaico bring-up，烧录前必须匹配 ESP32-S31。当前 ESP-IDF preview 若自身
  源码/header 不同步，失败应记录为 SDK blocker，不在项目中 patch 本机 ESP-IDF。

`host/`、`device/`、`work/`、`runtime/`、`platform/` 和 `conformance/` 各自维护子系统 CMake 清单；
`host/CMakeLists.txt` 汇总 Controller、System UI 与 Host 时间支持，不为 `abi/` 等叶子目录继续增加小型
CMake 文件。
