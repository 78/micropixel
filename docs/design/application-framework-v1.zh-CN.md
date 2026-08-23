# AI 原生嵌入式 Application Framework 架构总结

**版本：V1**  
**定位：面向 AI 生成应用的、语言无关、跨芯片、跨设备的嵌入式 Application Framework**  
**首版目标平台：ESP32-P4 / Metalio-Claw4，后续平台另行评估**

> **实施说明（2026-08-23）：** 产品已进入多 App/System Shell 阶段，但设备仍严格限制为最多一个
> Guest `AppSession` 存在。第一版已落地长驻 Host、可复用 WAMR、单 Session 完整销毁、双 Bundle 目录、
> Flash 封面、挂起截图、恢复/切换、上下边缘系统手势、状态层与性能蒙层；在线安装、网络服务和真机交互回归
> 继续按里程碑推进。最新执行范围见
> [MicroPixel Application Runtime 开发里程碑](../roadmap/development-milestones.zh-CN.md)。
> 当前 Guest C++ Public API 已进一步确定为“`Application` capability façade → copyable Service
> View → move-only Resource”；本文中的 Module/Service 长期讨论不得覆盖该运行时对象分类。
> Guest–Host 的当前 ABI 稳定、Service 独立版本、控制面/数据面和兼容性规则见
> [Guest–Host Service ABI 稳定与演进规范](guest-host-service-abi.zh-CN.md)，该规范优先于本文早期
> 的泛化 `invoke` 示例。

---

## 1. 项目定位

我们要做的核心不是“另一个操作系统”，而是一套类似 **.NET / WinRT + DirectX + Android Framework** 的嵌入式 Application Framework。

底层 OS、RTOS、芯片、驱动只是 Backend；真正稳定并对 App 暴露的是 Framework API。

```text
                    AI / Developer
                          │
                    C++23 / Rust / ...
                          │
                          ▼
                Framework SDK / Typed IDL
                          │
══════════════════════════════════════════════
              AI Device Framework
══════════════════════════════════════════════

 App Runtime
 UI
 Graphics
 Audio
 Camera
 Sensors
 Networking
 Cloud Resources
 AI Services
 Hardware
 Notifications
 Power
 Storage
 Diagnostics

══════════════════════════════════════════════
               Platform Adapter
══════════════════════════════════════════════

      NuttX / ESP-IDF / FreeRTOS / other OS

══════════════════════════════════════════════
                   Hardware
```

核心思想：

> **Framework 是主体，OS/SoC 是 Backend。**


当前进一步收敛出的接口原则：

> **标准 Public API 必须 Typed；Runtime Host ABI 必须窄；Service 必须可发现/可版本化；高频和大块数据必须绕开通用 RPC。**

---

## 2. 最重要的架构原则

### 2.1 Typed IDL 是真正的稳定接口

真正稳定的是：

```text
Typed IDL
+
Resource / Capability Model
+
Canonical ABI
```

而不是：

```text
C++ class ABI
```

C++、Rust、AssemblyScript 等语言只是 IDL 的不同 generated binding。

```text
               Typed IDL
                  │
       ┌──────────┼───────────┐
       ↓          ↓           ↓
   C++ binding Rust binding  AI Schema
       │          │
       └──────┬───┘
              ↓
            Wasm
              ↓
         WAMR Host ABI
              ↓
      Framework Runtime
```

IDL 应支持至少：

- `struct`
- `enum`
- `variant`
- `optional`
- `result<T, E>`
- `list`
- `string`
- `resource`
- ownership / borrow
- events
- asynchronous request
- interface version
- capability declaration


### 2.2 三层接口架构：Typed Public API → Narrow Host ABI → Service Router

借鉴 ESP-Brookesia 的通用 Service Bridge 思想，但不让 AI 直接面对字符串 Service 调用和 JSON 参数。

推荐分成三层：

```text
                    AI Generated App
                          │
                    Typed C++ SDK
                          │
                   IDL Generated Code
                          │
══════════════════════════════════════════════
              Stable Runtime Host ABI
══════════════════════════════════════════════

   invoke
   subscribe
   unsubscribe
   result_get / result_release
   resource_release
   graphics_submit
   diagnostics_state

══════════════════════════════════════════════
                          │
                    Service Router
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
       UI               Audio             Device
     Service           Service            Service
        │                 │                 │
        ▼                 ▼                 ▼
      Backend           Backend            HAL
```

核心原则：

> **Public API 强类型，Host ABI 泛化。**

这意味着新增一个标准 Service 或方法时，不一定新增一个新的 Wasm import。

例如 IDL：

```text
interface device.power {
    get_battery_state()
        -> result<BatteryState, Error>;

    set_charging_enabled(
        enabled: bool
    ) -> result<(), Error>;
}
```

C++ SDK：

```cpp
auto battery =
    device().power().battery_state().value();

device()
    .power()
    .set_charging_enabled(true);
```

IDL generator 最终可以 lower 为：

```text
interface_id = DEVICE_POWER
method_id    = SET_CHARGING_ENABLED
payload      = typed binary payload
```

再通过一个稳定的：

```text
framework_invoke(...)
```

进入 Framework Router。

这样同时得到：

- C++ 编译期类型检查
- AI 编译错误自动修复
- 少量、稳定的 WAMR imports
- Service 可扩展
- 多语言 binding 复用同一 Host ABI

### 2.3 标准 Typed API 与 Dynamic Service Bridge 共存

标准 Framework API 必须由 Typed IDL 定义，例如：

```text
ui
 graphics
 audio
 camera
 resource
 cloud
 sensor
 hardware
 system
 diagnostics
```

AI 正常生成 App 时只使用 typed SDK。

同时可以保留动态扩展入口，用于：

- 第三方 Service
- Plugin
- 调试工具
- 尚未进入标准 Framework 的实验接口
- AI 运行时发现的新扩展能力

例如：

```cpp
auto schema =
    framework::dynamic::service_schema("foo");

auto result =
    framework::dynamic::call(
        "foo",
        "bar",
        json
    );
```

动态接口依赖机器可读 Schema；它是 **escape hatch**，不是标准 App API。

因此：

```text
标准能力
→ Typed IDL
→ 高可靠 / 高性能 / 可静态检查

动态扩展
→ Schema + Generic Bridge
→ 高灵活性 / 高扩展性
```

---

## 3. V1 编程语言与执行模型

### 3.1 V1 只做 C++23 SDK

V1 不同时维护多语言。

```text
AI
 ↓
Restricted C++23
 ↓
Clang → Wasm
 ↓
wamrc → AOT
 ↓
WAMR
```

以后增加 Rust：

```text
IDL
 ├── C++ generator
 └── Rust generator
```

底层 Framework Runtime 不需要修改。

### 3.2 Restricted C++23 Profile

推荐：

- RAII
- `String`
- `Vector`
- `Optional`
- `Result<T,E>`
- `Span`
- value type
- strong enum
- Framework resource objects

尽量禁止或不向 AI 暴露：

- raw `new/delete`
- 裸 ownership pointer
- `std::thread`
- `pthread`
- `mutex`
- arbitrary `ioctl`
- MMIO
- LVGL API
- arbitrary `/dev/*`
- exception
- RTTI
- complex template metaprogramming

推荐编译选项：

```text
-fno-exceptions
-fno-rtti
```

---

## 4. WAMR 作为默认 App Runtime

MMU 不再是 Framework 的必要条件。

AI 生成代码编译为 Wasm，WAMR 提供 App sandbox。

### 4.1 当前产品的执行单位

不是“每 App 一份完整 WAMR”，也不是让多个 Guest 同时驻留。当前产品约束为：

- 可安装 Bundle 数量为 N；
- WAMR VMcore 全局只有一份；
- 任意时刻 `AppSession` 数量只能是 0 或 1；
- 启动另一个 App 前，必须先完整销毁现有 Session。

```text
WAMR VMcore                 × 1（Host 生命周期）
Installed Bundle            × N（Flash）
Loaded module               × 0..1（AppSession）
Module instance / exec_env  × 0..1（AppSession）
GuestContext / resource     × 0..1（AppSession）
```

结构：

```text
Application Runtime Host
│
├── WAMR Core（长驻、可复用）
├── AppSession（最多一个）
│    ├── Bundle mapping / Wasm Module
│    ├── Module Instance / Exec Env / Linear Memory
│    ├── GuestContext / Capability / Resource / Event Queue
│    └── Trap / watchdog / teardown boundary
└── Host System Shell（不属于 Guest）
     ├── App Hall
     ├── Status Layer
     └── System Gesture Router
```

### 4.2 ESP-IDF

当前 ESP32-P4 产品路径：

```text
一个 Framework Firmware
│
├── Host System Shell / Device services
├── WAMR Core（一次初始化）
└── 最多一个 WAMR App task → AppSession
```

Framework 内部的音频、资源等服务仍可使用自己的有界 worker；“服务并发”不等于“多个 Guest 并发”。

### 4.3 NuttX

推荐 V1：

```text
AppHost Process
│
├── WAMR Core
├── pthread → App A
├── pthread → App B
└── pthread → App C
```

在有 MMU 的高端设备上可以提供更强隔离：

```text
Dedicated AppHost Process
└── One Wasm App
```

是否“一 App 一 OS 进程”应该是 **Isolation Policy**，不是 Framework 强制规则。


### 4.4 WAMR Host ABI 应保持窄而稳定

WAMR import 不应该随着 Framework API 数量线性增长。

V1 可优先定义一组很小的宿主 ABI，例如概念上：

```text
framework_invoke(...)
framework_subscribe(...)
framework_unsubscribe(...)

framework_result_len(...)
framework_result_copy(...)
framework_result_free(...)

framework_resource_release(...)
framework_graphics_submit(...)
framework_debug_state(...)
```

具体名称可以后续调整，但职责要稳定。

Typed IDL generator 自动负责：

```text
C++ typed call
 ↓
serialize typed payload
 ↓
Host ABI
 ↓
validate
 ↓
Service Router
 ↓
Service method
```

所有 Wasm pointer / length 的 native marshaling 都应尽可能自动生成并集中审计，避免手写 Host Binding 成为安全漏洞。

---

## 5. App 并发模型

### 5.1 Single-threaded App Model

每个 App：

```text
1 Wasm instance
1 App task/thread
1 event queue
```

Framework 内部可以多线程，但 App callback 串行执行。

```text
Touch
Timer
HTTP
Audio
Resource
Frame
  │
  ▼
App Event Queue
  │
  ▼
Single App Task
```

优点：

- AI 不需要处理 data race
- 不需要 mutex
- 生命周期简单
- 资源 ownership 清晰
- 调试容易

---

## 6. 游戏 API：同步世界 + 异步系统世界

### 6.1 游戏主循环由 Framework 驱动

> **当前实施修订：** 第一阶段改为 Guest 持有 `WaitEvent()` 事件循环，使用 Timer 驱动 Snake
> 等低频逻辑；Host 持续驱动的 `on_frame()` 不再是 Snake 之前的必选模型。显示同步能力以后可按需
> 增加。Public SDK 使用 AI-first 任务层：配置好的资源由 `every()/after()` 一步获得，typed
> event 通过 `TimerFrom()` 等来源感知方法匹配；程序、Runtime 或 ABI 故障在发生点 panic，
> 只有失败属于正常业务结果的接口才返回 `Result<T>` 或 typed event。
> 详见 [当前开发里程碑](../roadmap/development-milestones.zh-CN.md) 和
> [Guest C++ SDK](../../guest/sdk/README.md)。以下内容保留为早期方案记录。

不要让 AI 写：

```cpp
while (true) {
    update();
    render();
}
```

而是：

```cpp
void on_frame(FrameContext& frame)
{
    update(frame.delta_time());
    render(frame.graphics());
}
```

Framework 根据：

- foreground/background
- refresh rate
- power mode
- thermal state
- visibility

决定 frame cadence。

### 6.2 图形 API：同步 Command Recording

```cpp
gfx.clear(...);
gfx.blit(...);
gfx.draw_text(...);
gfx.present(...);
```

App 看起来同步，底层可以异步执行。

```text
App
 ↓
2D Command Buffer
 ↓
ui_server
 ↓
PPA / DMA / CPU
 ↓
Display
```

### 6.3 慢操作必须异步

适合异步的能力：

- HTTP
- Cloud Resource
- AI API
- 大型文件
- Model
- Camera capture
- 权限请求
- 网络资源

推荐：

```cpp
auto req = resources().request("background");

void on_resource_ready(ResourceHandle resource)
{
    ...
}
```


### 6.4 异步语义不要求异步 Wasm Host Function

Framework 可以把底层 Host ABI 保持为同步调用：

```text
submit request
→ 立即返回 RequestHandle
```

真实工作在 Framework 后台执行：

```text
HTTP / Cloud / Camera / AI
        ↓
background worker/service
        ↓
completion event
        ↓
App Event Queue
        ↓
on_request_complete(...)
```

因此即便底层 WAMR Host Function 本身是同步 Native Function，App 仍然拥有完整的异步编程模型。

这特别适合：

```text
One App
= One Event Loop
= Serial Callbacks
```

---

## 6A. 三种跨 Wasm / Framework 数据路径

不能让所有能力都通过一种 Generic RPC 传输。

推荐明确分三条路径：

```text
                   Framework Host Boundary

          ┌────────────────┼────────────────┐
          │                │                │
          ▼                ▼                ▼
      Typed RPC       Command Buffer    Resource Handle

      控制面             高频热路径          大块数据
```

### Typed RPC — 控制面

适合：

- Camera.open
- Audio.play
- HTTP request
- PowerLease.acquire
- Notification.post
- Sensor.subscribe
- Resource.open

IDL 负责类型检查和序列化。

标准接口优先使用紧凑的 typed/binary payload，而不是让 AI 手写 JSON。

### Command Buffer — 高频渲染

适合：

```text
CLEAR
FILL
BLIT
DRAW_TEXT
PRESENT
```

App 一帧构造一个 command batch，再一次提交：

```text
Wasm
 ↓
graphics_submit(command_buffer)
 ↓
Graphics Runtime
 ↓
PPA / DMA / CPU
```

不要每个 Sprite 都做一次 JSON Service Call。

### Resource Handle — 大块数据

适合：

- CameraFrame
- AudioStream
- Texture
- Surface
- Model
- Image Resource
- Shared Buffer

Wasm 只持有 opaque handle：

```text
Texture #21
CameraFrame #87
Surface #12
```

真正的大块数据留在 Native / Shared Resource Layer，避免频繁复制进 Wasm linear memory。

---

## 7. UI Server

普通 App 不知道 LVGL 的存在。

第一版不要拆：

```text
Window System
LVGL Server
Compositor
```

为多个进程。

先统一为一个：

```text
ui_server
```

内部逻辑分层：

```text
ui_server
│
├── IPC / Session Manager
├── Window / Scene Manager
├── Input Router
├── System Gesture Router
│
├── Widget Runtime
│    └── LVGL Backend
│
├── Font Service
├── Text Renderer
│
├── Surface Manager
│    └── Buffer Queue
│
├── Graphics Command Processor
├── Compositor
├── Render Scheduler
│
└── Display Backend
```

### 7.1 Window Manager ≠ LVGL

Canonical objects：

```text
Window
Widget
Surface
```

不是：

```text
lv_obj_t
```

LVGL 只能出现在：

```text
ui/widget/lvgl_backend/
```

---

## 8. 两条 UI / Graphics 路线

### 8.1 Widget App

适合：

- 天气
- 股票
- 设置
- 音乐播放器
- 文件管理
- AI Chat
- 万年历

```text
App
 ↓
Widget IDL
 ↓
UI Object Tree
 ↓
LVGL
 ↓
Display
```

典型 API：

```cpp
auto win = ui().window();

auto root = win.column();

root.text("Weather");

root.button("Refresh")
    .on_click([this] {
        refresh();
    });
```

### 8.2 Game / Surface App

适合：

- Tetris
- Snake
- 2048
- 模拟器
- Camera Preview
- Video
- 实时波形

```text
App
 ↓
Surface / 2D Commands
 ↓
ui_server
 ↓
PPA / CPU / DMA
 ↓
Display
```

不推荐默认把完整 framebuffer 暴露给 Wasm。

优先：

```cpp
frame.clear(...);
frame.blit(...);
frame.draw_text(...);
```

而不是：

```cpp
uint16_t *pixels = surface.map();
```

因为 Wasm ↔ Host 大块内存传输成本高。

---

## 9. Compositor

Compositor 必须逻辑独立，但 V1 不单独拆进程。

它负责：

- Scene evaluation
- z-order
- visible region
- opaque region
- dirty region
- direct scanout
- overlay composition
- PPA/CPU backend selection

典型 fast paths：

```text
Widgets Only
→ LVGL Direct

Fullscreen Opaque Surface
→ Direct Scanout

Surface + System Overlay
→ PPA / CPU Composition
```

---

## 10. 系统字体与文本渲染

字体必须是 `ui_server` 的共享系统能力。

```text
ui_server
│
├── Font Manager
├── Text Renderer
├── Glyph Cache
└── Font Resource Resolver
```

Widget、System Shell、Game 共享：

- 字体
- fallback
- glyph rasterizer
- glyph cache
- text metrics
- Unicode handling

### 10.1 游戏文本

游戏 API：

```cpp
frame.draw_text(
    "Score: 1200",
    {8, 8},
    TextRole::GameHud
);
```

内部：

```text
DRAW_TEXT
 ↓
System Text Renderer
 ↓
Glyph Cache
 ↓
PPA / CPU blend
 ↓
Surface
```

游戏不自己携带和 rasterize 字体。

---

## 11. Resource Model

App 的“资源空间”不等于本地文件系统。

统一：

```text
ResourceRef
ResourceStream
```

Resource Provider：

```text
package://
system://
file://
cache://
content://
cloud://
https://
```

### 11.1 API

```cpp
ui.image(resource("background"));

audio.play(resource("bgm"));

ai.load_model(resource("vision-model"));
```

不要让 App 到处写固定 URL。

---

## 12. Cloud Resource Service

用户通过：

- Store 发布 App
- Web IDE 开发
- AI 生成 App

后，可以使用平台级 Cloud Resource。

```text
App
 ↓
ResourceRef
 ↓
resourced
 ↓
cloud_service
 ↓
HTTP/3 / QUIC
 ↓
Cloud CDN / Store / AI
```

### 12.1 HTTP/3

App API 不暴露 HTTP/3。

HTTP/3 只是 Backend。

Device 可以维护少量 QUIC Session，并在单连接中复用多个 stream：

```text
HTTP/3 Connection
├ font
├ image
├ audio
├ model chunk
├ AI
└ telemetry
```

可以按 traffic class 分：

```text
interactive
media
bulk
```

---

## 13. Adaptive Resource

ResourceRef 表示“逻辑内容”，不是固定文件。

云端根据设备能力选择 representation。

例如：

```text
background

├ original.png
├ 320x240-jpeg-q75
├ 480x320-jpeg-q80
└ thumbnail
```

Device Descriptor：

```yaml
display:
  width: 320
  height: 240
  preferred_format: rgb565

graphics:
  jpeg:
    hardware_decode: true

  accelerator:
    scale: true
    rotate: true
    blend: true
```

云端自动下发最合适版本。

---

## 14. ESP32-S31 图形加速

S31 可利用：

```text
JPEG HW Codec
PPA
2D-DMA
```

典型 Cloud Image pipeline：

```text
Cloud JPEG
 ↓
HTTP/3 stream
 ↓
S31 JPEG Decoder
 ↓
RGB565
 ↓
PPA scale / rotate / blend
 ↓
Surface
 ↓
Display
```

Camera 上传：

```text
Camera
 ↓
JPEG HW Encode
 ↓
HTTP/3
 ↓
Cloud Vision
```

---

## 15. 多媒体 API

### 15.1 Audio

控制面：

```text
IDL
```

数据面：

```text
Native RingBuffer / Stream
```

App：

```cpp
audio.play(resource("bgm"));
```

底层负责：

- decode
- mixer
- audio focus
- volume
- route
- buffer
- underrun

### 15.2 Camera

App：

```cpp
auto camera = cameras().open(CameraRole::Main);

camera.attach(preview_surface);
```

普通 preview 不经过 Wasm frame copy。

如果 AI Vision：

```text
CameraFrameHandle
 ↓
Vision Service
```

传 opaque handle，不传大块 pixel buffer。

---

## 16. Network API

### 普通 App

默认提供：

```text
Http
WebSocket
Cloud Resource
AI Service
```

不默认给 raw socket。

### 高权限 App

可申请：

```text
network.raw_socket
```

底层依然可以使用：

- BSD socket
- TCP
- UDP
- HTTP/2
- HTTP/3
- QUIC

---

## 17. 文件系统

普通私有文件继续使用 POSIX：

```text
open
read
write
stat
opendir
```

App 只能访问自己的 namespace。

系统级跨 App 内容交换通过：

```text
ContentHandle
File Picker
Share
Intent
```

而不是暴露另一个 App 的真实路径。

---

## 18. Board Resource Model

Framework 要屏蔽不同开发板的硬件差异。

App 使用：

```cpp
hardware::i2c("expansion-i2c");

sensors::open(SensorRole::Motion);

devices::open("status-led");
```

而不是：

```text
/dev/i2c1
GPIO18
QMI8658 address
```

Board Descriptor：

```yaml
display.main:
  type: display

motion.main:
  type: imu

expansion-i2c:
  type: i2c
  exposed: true

GPIO4:
  exposed: true
```

---

## 19. Managed Raw Hardware

开发板必须允许用户扩展。

提供两级：

### Semantic API

```text
status-led
motion.main
display.main
speaker
camera.main
```

### Managed Raw Hardware

```text
hardware.gpio
hardware.i2c
hardware.spi
hardware.uart
hardware.pwm
hardware.adc
```

不要默认开放：

```text
/dev/*
MMIO
pinmux register
```

资源采用 lease：

```text
acquire
 ↓
permission check
 ↓
ownership
 ↓
pinmux/config
 ↓
lease
```

App 退出 / 崩溃：

```text
revoke leases
 ↓
hardware safe state
```

---

## 20. 跨芯片适配

目标：

```text
Same Framework API
Same Wasm App
Different Platform Adapter
```

架构：

```text
               AI App
                C++23
                  │
                  ▼
                Wasm
                  │
════════════════════════════════
         AI Device Framework
════════════════════════════════
                  │
         Platform Adapter
                  │
    ┌─────────────┼─────────────┐
    ▼             ▼             ▼
 ESP32-S3       BK7258        RP2040
 Xtensa          ARM          Cortex-M
    │             │             │
 WAMR           WAMR           WAMR
```

需要两个适配层：

### Runtime Platform Adapter

- task/thread
- timer
- allocator
- filesystem
- WAMR port
- AOT loader

### Device Platform Adapter

- LCD
- Touch
- Audio
- Camera
- Sensor
- GPIO
- I2C
- SPI
- accelerator
- JPEG

---

## 21. Device Profiles

不是所有设备都必须支持同样能力。

推荐：

```text
Tiny
Standard
Rich
```

例如：

### Tiny

RP2040 类：

- 单前台 App
- 小 Wasm memory
- GPIO/I2C
- 简单 UI
- 简单游戏

### Standard

ESP32-S3 类：

- UI
- Touch
- Audio
- Camera
- Motion
- Network
- 多 App
- Cloud Resource

### Rich

ESP32-S31 类：

- 更高图形性能
- JPEG HW
- PPA
- 2D-DMA
- 更大资源
- 高级 Camera
- 更复杂游戏

---

## 22. App Lifecycle

当前产品只保留实际需要的三个状态，不设置 `Background`，也不引入一组暂时没有消费者的 Guest callback：

```text
NotRunning ── start ──> Foreground
     ▲                     │  ▲
     │                     │  │
     └────── stop ─────────┘  │
                           suspend
                              │
                              ▼
                          Suspended
                              │
                            resume
                              │
                              └──────> Foreground
```

语义：

- `start`：创建唯一 AppSession，Guest 的 `main()` / `__micropixel_start` 就是启动入口；
- `suspend`：暂停 Guest task 和 Guest 输入/帧提交，Session 与资源仍保留；
- `resume`：Host 直接恢复暂停前 retained Guest view，再向事件队列最前面投递 typed Resume event；恢复同一个
  Session，不重新调用启动入口，Guest 可据此完整重画；
- `stop`：Host 先把 typed Stop event 放到事件队列最前面；Guest 正常返回后销毁 GuestContext、exec-env、
  instance、module 与 Bundle mapping，500ms 内不响应才强制 terminate；
- Guest 正常退出、Trap 或 watchdog 超时都归一为 `stop → NotRunning → App Hall`。

当前不定义 `on_start`、`on_foreground`、`on_background`、`on_suspend`、`on_resume` 或 `on_stop` callback。
已经出现的重画与安全切换需求只增加 `EventType::kResume` / `EventType::kStop`，不扩展成暂时没有消费者的
一整套 lifecycle callback。

第一版已经完成 `Foreground ↔ Suspended` 与 `Foreground → NotRunning`。挂起只保留唯一 Session；切换
App 时仍必须先 stop 旧 Session，不能用“多个 Guest 同时运行”替代。

---

## 23. Power Model

`powerd` 负责：

- CPU frequency
- display
- backlight
- Wi-Fi
- Bluetooth
- audio
- camera
- sensor
- PSRAM
- peripheral power
- sleep
- deep sleep

采用 Lease：

```cpp
auto lease =
    power.acquire(PowerRequirement::ScreenOn);
```

系统决定是否授予和何时降级。

---

## 24. Background Job / Alarm

不要：

```cpp
while (true) {
    sleep(1800);
    refresh();
}
```

而是：

```text
scheduler.schedule_periodic(...)
```

系统可以 suspend App，到时间再唤醒。

---

## 25. System Shell

`SystemShell` 是 Firmware 内的 Host 原生系统组件，不是另一个 WAMR Guest，也不写入内核。这样它能在
Guest 不存在、崩溃或被暂停时继续绘制 UI、接管手势和回收资源，同时不占用第二份 Guest Runtime。

包含：

- App Hall：列出 Flash 中的 Bundle；未运行 App 使用封面，挂起 App 使用 Host 捕获的最后一帧并显示运行标志；
- 全屏 Status Layer：Wi-Fi/4G/电池/内存/Flash、亮度和音量滑杆及常用开关；
- System Gesture Router 和单 App 切换流程；安装/卸载与网络配置继续作为大厅管理能力实现；
- Host 性能蒙层：打开 FPS 后，在最终前台画面叠加实际呈现 FPS 与整机聚合 CPU 使用率。

Status Layer 打开时 Guest 已暂停，因此其中不显示 CPU 使用率。系统 UI 优先使用 LVGL primitive、小型
Flash 常驻 glyph 和纯色/半透明图层，不加载大块背景贴图，也不做高成本实时模糊。

---

## 26. System Gesture

Raw touch 先经过：

```text
System Gesture Recognizer
```

典型：

```text
最顶部下滑命中 → 自动完整展开全屏 Status Layer，并暂停 Guest
最底部上滑命中 → 暂停 Guest、捕获最后一帧并缩回 App Hall 卡片
边缘手势 → Back/System
普通触摸 → 当前 App
```

系统手势优先于 Guest 输入；没有“拉到一半”的抽屉状态。点击大厅中唯一的运行卡片时反向展开并恢复原
Session；点击另一个 App 时，先 stop 当前 Session，再 start 新 App。

---

## 27. 无桌面模式

支持：

```text
Shell
SingleApp
Kiosk
Headless
Recovery
```

例如：

```yaml
boot:
  mode: single_app
  app: com.example.weather
```

此时：

```text
system services
+
user app
```

`system_shell` 可以不启动。

必须保留：

- safe mode
- recovery gesture/button
- crash loop fallback
- rollback

---

## 28. Notification

App：

```cpp
notification.post({
    .title = "Weather",
    .body = "10 minutes until rain"
});
```

架构：

```text
App
 ↓
notificationd
 ↓
system_shell
```

通知支持：

- title
- body
- icon
- action
- progress
- priority
- sound

---

## 29. Intent / Share / Open With

高层跨 App 协议：

```text
open_url
open_file
share
launch
view content
```

不要允许任意 App 互相直接访问私有资源。

跨 App 内容使用：

```text
ContentHandle
```

---

## 30. Account / AI Service

账号 Token 不给 App。

系统：

```text
accountd
 ↓
cloud_service
 ↓
AI / Store / Share
```

AI Service：

```text
ai.chat
ai.text.generate
ai.image.generate
ai.vision.analyze
ai.speech.recognize
ai.speech.synthesize
```

App 不知道底层 AI Provider。

---

## 31. Package / Store

App package：

```text
manifest
app.wasm / app.aot
icon
bootstrap assets
resource metadata
signature
```

Store 可保存：

```text
canonical app.wasm
```

设备安装时：

```text
app.wasm
 ↓
target-specific AOT
```

例如：

```text
Xtensa
ARM
RISC-V
```

---

## 32. Trust / Security

需要：

- Secure Boot
- Firmware signature
- App signature
- capability
- permission
- sandbox
- resource lease
- secrets vault
- credential isolation
- developer mode

可信边界：

```text
Wasm App
 ↓
IDL Host Binding
 ↓
Framework
 ↓
OS / Driver
```

所有 Wasm ↔ Native pointer marshaling 应尽量由 IDL generator 自动生成。


### App Load 时建立 Capability Table

标准 App 不应该运行时自由探索并启动任意系统 Service。

安装/启动时根据 Manifest 和 Device Capability 建立：

```text
AppInstance
└── CapabilityTable
    ├ ui
    ├ audio
    ├ resource.cloud
    ├ sensor.motion
    └ hardware.i2c: expansion-i2c
```

每次 Host ABI 调用进入 Service Router 时先校验 CapabilityTable。

这样可以用于：

- Store 权限展示
- AI 生成阶段静态分析
- 运行时 enforcement
- 跨设备 compatibility 判断
- 安全审计

---

## 33. Diagnostics / AI Self-Debug

这是 Framework 的核心能力之一。

目标：

```text
Generate
Observe
Diagnose
Repair
Verify
```

### 33.1 普通日志

```cpp
log::info(...)
log::warn(...)
log::error(...)
```

表示：

> 发生了什么。

### 33.2 `debug::state`

```cpp
debug::state("piece.x", piece_.x);
debug::state("score", score_);
```

表示：

> 当前内部状态是什么。

这是 **Framework Diagnostics API**，不是 WAMR 原生 API。

Host 通过当前 module instance 知道 App 身份。

State 使用覆盖式存储：

```text
piece.x = 5
piece.y = 12
score = 1200
```

不是每次产生永久日志。

### 33.3 `debug::event`

```cpp
debug::event("line_cleared", {
    {"count", 4},
    {"score", score_}
});
```

用于业务关键事件。

---

## 34. Framework 自动观测

无需 App 主动输出：

- Wasm trap
- memory
- CPU
- FPS
- dropped frames
- audio underrun
- resource usage
- UI tree
- screenshot
- input trace
- resource trace
- framework calls
- network metadata

---

## 35. Debug Bundle

```text
DebugBundle
├── app
│   ├ app_id
│   ├ version
│   └ build_id
│
├── build
│   ├ compiler
│   ├ SDK version
│   └ IDL version
│
├── error
│   ├ compile error
│   ├ wasm trap
│   └ framework error
│
├── logs
├── debug_state
├── debug_events
├── event_trace
├── framework_trace
├── resource_state
├── metrics
│
├── ui
│   ├ screenshot
│   └ semantic_tree
│
└── device
    ├ board
    ├ capabilities
    ├ memory
    └ firmware
```

---

## 36. Flight Recorder

每 App 在 RAM 内维护 ring buffer：

```text
最近 Framework calls
最近 Input events
最近 Logs
最近 Resource events
最近 Metrics
```

发生异常时：

```text
freeze
 ↓
Debug Bundle
 ↓
Cloud
 ↓
AI Repair
```

---

## 37. Screenshot + Semantic UI Tree

Screenshot 用来判断视觉结果。

UI Tree 用来提供精确结构。

```text
Window 320×240

Button#14
  rect=[282,210,74,32]
  clipped=true
```

AI 可以同时知道：

- 看起来哪里错
- 哪个 Widget 错
- 具体坐标/尺寸错多少

---

## 38. Game Debug

游戏无 Widget Tree 时，使用：

```text
Screenshot
+
Game debug::state
+
Input Trace
+
Frame Metrics
```

例如：

```text
piece.x = -1
piece.width = 2
board.width = 10
```

再结合：

```text
WASM_TRAP: out of bounds
```

AI 可自动定位 bug。

---

## 39. AI 自动修复闭环

```text
User Requirement
      ↓
AI generates C++
      ↓
Compile
      ↓
Compiler errors?
  ┌── yes → AI repair ──┐
  │                     │
  └───────── no ────────┘
      ↓
Simulator
      ↓
Install Device
      ↓
Run
      ↓
Collect Debug Bundle
      ↓
AI Diagnose
      ↓
Modify
      ↓
Compile
      ↓
Replay
      ↓
Screenshot / Semantic Check
      ↓
Promote Version
```

---

## 40. 三层 Debug

### Level 1 — Cloud Compile

处理：

- type error
- missing method
- link error
- invalid capability API

### Level 2 — Host Simulator

处理：

- UI
- game logic
- event replay
- resource mocks
- screenshot
- semantic layout
- basic performance

### Level 3 — Real Device

处理：

- real touch
- camera
- audio
- GPIO/I2C
- network
- power
- memory
- PPA/JPEG
- LCD bandwidth

---

## 41. Host Simulator

同一套 Framework API：

```text
Framework
 ↓
Host Adapter
 ↓
SDL / Desktop Window / PC Audio / Webcam / Mock Sensor
```

AI 可以大部分时间在云端/PC 模拟器调试，最后再上真机。

---

## 42. Board / Device Descriptor 也是 AI Context

AI 在生成代码前看到：

```text
Display
Touch
Audio
Camera
Motion
GPIO
I2C
PPA
JPEG
RAM
Flash
Network
```

AI 不需要查 datasheet。

例如：

```text
用户：
“接 BME280 做天气站”

AI：
发现 expansion-i2c
→ 生成 I2C driver logic
→ 生成 UI
→ 生成 Cloud Weather feature
```

---

## 43. Golden Apps

V1 应用测试至少覆盖：

1. Tetris
2. Snake / 2048
3. Music Player
4. Shake Lottery
5. AI Voice Chat
6. Camera + Vision
7. AI Image Viewer/Generator
8. Weather
9. Stock
10. Calendar
11. GPIO/I2C Light Controller

这些 App 反推 Framework API，而不是先幻想一个无限大的 SDK。

---

## 44. V1 最小 Framework API

### Core

```text
Application
Timer
Log
Diagnostics
```

### UI

```text
Window
Container
Text
Button
Image
List
TextInput
Slider
Input
```

### Graphics

```text
Surface
Frame
Texture
Fill
Blit
DrawText
Present
```

### Resource

```text
ResourceRef
ResourceStream
Request
```

### Media

```text
Audio
Camera
```

### Sensor

```text
Motion
```

### Network

```text
Http
WebSocket
```

### Hardware

```text
GPIO
I2C
PWM
```

### System

```text
Notification
PowerLease
Intent
```


### Dynamic Extension

```text
ServiceSchema
DynamicCall
DynamicSubscription
```

仅用于动态/第三方扩展，不作为普通 AI App 的默认调用方式。

### Runtime Host ABI（内部）

```text
invoke
subscribe
unsubscribe
result
resource_release
graphics_submit
diagnostics_state
```

这是 Runtime 与 Framework 的内部稳定边界，不直接作为 AI 编程 API。

---

## 45. 推荐项目目录

```text
framework/
├── idl/
│   ├── compiler/
│   └── interfaces/
│
├── sdk/
│   └── cpp/
│
├── runtime/
│   ├── wamr/
│   ├── app_host/
│   ├── host_abi/
│   ├── service_router/
│   ├── dynamic_bridge/
│   ├── capability/
│   └── resource/
│
├── ui/
│   ├── scene/
│   ├── window/
│   ├── input/
│   ├── widget/
│   │   └── lvgl/
│   ├── font/
│   ├── surface/
│   ├── compositor/
│   └── display/
│
├── media/
│   ├── audio/
│   └── camera/
│
├── cloud/
│   ├── resource/
│   ├── http3/
│   └── ai/
│
├── system/
│   ├── appmgr/
│   ├── power/
│   ├── notification/
│   ├── account/
│   ├── settings/
│   ├── permissions/
│   └── crash/
│
├── diagnostics/
│
├── shell/
│
└── platform/
    ├── nuttx/
    ├── esp_idf/
    ├── host/
    └── boards/
```

---

## 46. 建议 V1 实施顺序

> 本节是 V1 形成时的宽范围建议，已由更细化的
> [当前开发里程碑](../roadmap/development-milestones.zh-CN.md) 取代，不作为当前排期。

### Milestone 1 — Runtime

```text
C++ → Wasm → WAMR
```

实现：

- App load
- App instance
- event queue
- trap
- restart

### Milestone 2 — Typed IDL + Generic Host Bridge

实现：

- resource
- struct
- enum
- variant
- optional
- result
- event
- interface/method ID
- C++ generator
- typed serializer
- host binding generator
- narrow Host ABI
- Service Router
- Service Schema dump
- Dynamic Service Bridge（最小版）

### Milestone 3 — UI

实现：

```text
Window
Text
Button
Column
Input
```

跑通：

```text
Hello App
Calculator
Weather UI
```

### Milestone 4 — Game

实现：

```text
Surface
Frame
Fill
Blit
DrawText
Touch
```

跑通：

```text
Tetris
Snake
```

### Milestone 5 — Resource / Cloud

实现：

```text
ResourceRef
Request
Cache
HTTP/3
Image
Audio Resource
```

### Milestone 6 — Media / Hardware

实现：

```text
Audio
Motion
GPIO
I2C
Camera
```

### Milestone 7 — Diagnostics

实现：

```text
log
debug::state
debug::event
Flight Recorder
Screenshot
UI Tree
Wasm Trap
Debug Bundle
```

### Milestone 8 — AI Repair

实现：

```text
Compile error → AI fix
Runtime trap → AI fix
Screenshot → UI fix
Event replay → verify
```

---

## 47. 目前最值得做的原型验证

### A. C++ → WAMR AOT

同一个 Tetris：

```text
ESP32-S3
ESP32-S31
```

对比：

- CPU
- RAM
- startup
- Wasm size
- AOT size
- graphics command throughput

### B. 每 App 一个 Instance + 一个 Task

验证：

```text
App A trap
→ A restart
→ B/C 继续
```

### C. Graphics Command API

验证：

```text
Fill
Blit
DrawText
Present
```

在：

```text
CPU
PPA
```

Backend 上运行。

### D. Resource Streaming

验证：

```text
HTTP/3
→ JPEG
→ decode
→ display
```

### E. Diagnostics

验证：

```text
debug::state
+
screenshot
+
input trace
+
Wasm trap
```

能否让 AI 自动修复 Tetris 边界 bug。

---

### F. Typed SDK vs Generic Bridge

用同一个 Device/Audio/HTTP Service 验证：

```text
AI Generated C++
→ Typed SDK
→ IDL serialization
→ Generic Host ABI
→ Service Router
```

测试：

- 编译期错误发现率
- Host ABI 调用开销
- binary payload vs JSON 开销
- Service 新增时 Runtime ABI 是否保持不变
- Dynamic Schema Service 是否能被 AI 正确理解和调用

---

## 48. 目前尚未完全定死的事项

需要通过 Prototype / Benchmark 决定：

- S3 上 WAMR Interpreter vs AOT 的真实收益
- 每设备支持多少并发 AppInstance
- 每 App 默认 linear memory 配额
- AppHost 是否需要多个 Runtime Pool
- Surface 是否允许部分 App 直接 map buffer
- HTTP/3 session 数量与 power policy
- Font cache / glyph atlas 具体策略
- App package 最终保存 Wasm 还是 AOT
- RP2040 Tiny Profile 的最低 Framework 范围
- BK7258 的 WAMR port/AOT 工程可行性
- 标准 typed payload 的 binary wire format（自研 / WIT-like / CBOR-like）
- Dynamic Service Bridge 是否使用 JSON、CBOR 或 typed schema codec
- Host ABI 的最终函数集合与版本演进规则
- System Shell V1 是否完整实现
- OTA / Account / Store 在 V1 还是 V2

---

# 49. 一句话总结

这套系统最终可以定义为：

> **一个面向 AI 生成应用的、Typed IDL 驱动、Wasm/WAMR 执行、跨芯片、跨设备、支持 UI/游戏/多媒体/云资源/真实硬件控制，并具备自动观测、调试、修复能力的嵌入式 Application Framework。**

它的核心不是某个芯片、某个 RTOS，也不是 LVGL。

真正的核心是：

```text
Typed Public IDL / SDK
+
Narrow Generic Runtime Host ABI
+
Service Router + Schema Registry
+
Wasm Application Runtime
+
Resource / Capability Model
+
Command Buffer Hot Path
+
Opaque Resource Handle Data Path
+
UI / Graphics Framework
+
Board Capability Model
+
Cloud Resource Model
+
Diagnostics / AI Repair Loop
```

其中最关键的新边界是：

> **AI 面对 Typed Framework API；WAMR 面对窄而稳定的 Generic Host ABI；Framework 内部通过 Service Router 分发；高频 Graphics 和大块 Media 数据使用专用通道。**

NuttX、ESP-IDF、ESP32-S3、ESP32-S31、BK7258、RP2040 都只是不同的 Runtime / Device Backend。
