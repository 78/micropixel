# Firmware 硬件分层与命名

本文定义 Firmware 中与开发板、外设和 Guest Service 有关的稳定词义。目标是让新增板型的开发者能够沿着
一条单向路径理解代码，而不需要猜测 `backend`、`provider` 或 `hardware` 在不同目录中的临时含义。

> Board 初始化 Driver，把可用 Peripheral、Controller 和 Presentation 登记给 Platform；Platform 将
> Peripheral 转成公开 Device，由 Service 管理，并通过 ABI Endpoint 提供给 Guest。

## 1. 依赖路径

```text
FirmwareApp
  └─ Platform                         # 跨板复用的装配、默认实现与 Registry
       └─ Board                       # 当前固件选择的具体 PCB/产品板
            ├─ Bus                    # I2C / SPI / I2S 等共享总线
            ├─ Driver                 # 芯片、控制器和寄存器驱动
            ├─ Peripheral             # 可登记的板级物理外设
            ├─ Controller             # 电源、亮度等板级控制
            └─ Presentation           # 转场、截屏、framebuffer 合成

BoardRegistration
       ↓
DeviceRegistry / PlatformServices
       ↓
Service
       ↓
ABI Endpoint
       ↓
Guest SDK
```

固定依赖方向仍是 `Runtime -> Device contracts <- Platform`。上述词义细化 Platform 内部和 ABI 前后的职责，
不允许 Runtime 直接依赖 Board、Driver 或 ESP-IDF 类型。

`platform/buses/` 与 `platform/drivers/` 是同级基础层，不互相包含。Bus 负责共享链路的所有权、排队和并发
控制；Driver 负责某颗芯片或控制器的寄存器与厂商 API。Driver 可以通过 Bus 执行事务，但 Bus 不依赖具体
Driver。纯 Driver 不实现 `device::` contract，也不依赖 Host UI 或 LVGL；将厂商触摸句柄转换为
`device::Input` 的可复用实现放在 `platform/input/`。具体 Board 的 CMake 明确选择它使用的 Driver 和 Input
实现，避免公共 Driver 清单隐式绑定某块板。

技术层也保持单向依赖：`platform/lvgl/display/` 只负责取得稳定像素并编码截屏，不解析 USB 或开发命令；
`platform/transports/` 可以调用该截屏接口并完成传输协议。Board 只组合两者，不把传输生命周期塞回显示层。

## 2. 固定词义

| 名词 | 唯一含义 |
|---|---|
| `Board` | 完整开发板或产品 PCB；拥有板型信息、wiring、初始化/关机次序和能力登记 |
| `Platform` | 不随板型变化的通用装配框架；拥有默认实现、公开服务集合和 `DeviceRegistry` |
| `Bus` | 多个器件共享的物理传输与调度，例如 I2C、SPI、I2S |
| `Driver` | 直接操作芯片、控制器、寄存器或厂商 SDK API 的实现 |
| `Peripheral` | Board 向上提供、可以登记为 Device 的物理外设；可来自 MCU 内部模块或外部器件 |
| `Channel` | Peripheral 内部的局部寻址值，例如 GPIO line、sensor channel；绝不是公开 `DeviceId` |
| `Controller` | 不按普通外设枚举的板级控制，例如 power、brightness |
| `Presentation` | Host 显示呈现机制，例如硬件转场、截屏、framebuffer/GRAM 合成 |
| `Device` | Platform 登记并可向 Guest 枚举的逻辑设备；拥有 opaque `DeviceId` |
| `Capability` | Device 或接口支持的操作事实，只用于 capability bit、flag 和查询结果 |
| `Registry` | 登记对象、分配身份并完成查找/路由；不实现产品业务 |
| `Service` | Host 中某项 Guest 能力唯一的业务、所有权和生命周期实现 |
| `Endpoint` | 只用于 Guest ABI request/response/event 的校验、编解码和分派 |
| `Adapter` | 仅转换两个已经存在的接口；不拥有业务规则或硬件生命周期 |

`interface` 和 `contract` 是设计语言，不作为默认类名后缀。`hardware` 只作为“真实硬件”的普通描述词，
不再作为所有板级对象的统一后缀。

## 3. Peripheral 的 MCU 语义

MicroPixel 不照搬 PC 的设备模型。这里的 Peripheral 同时包含 MCU 内部外设和板上外接器件，只要它表示
Board 向上提供的物理功能，并可能被登记成一个或多个逻辑 Device。

典型 Peripheral 包括 GPIO、sensor、display panel、touch、audio output、battery gauge、haptics 和 radio。
Guest↔Hall 转场、framebuffer 合成和截屏属于 Presentation；power/brightness 属于 Controller；System UI、
Registry 和 Service 都不是 Peripheral。

一个 Peripheral 可以暴露多个 Channel。例如一个 `GpioPeripheral` 暴露 line 5 和 line 15；Board 可将它们
显示为 `P5`、`P15` 或 `GPIO15`，Platform 再为每根允许开放的 line 分配独立 `DeviceId`。物理名称只用于
人机识别，不参与身份和路由。

## 4. 命名规则

- 具体 Board 使用 `<Product>Board`，选择入口使用 `ConfiguredBoard()`；
- 芯片实现使用芯片/机制加角色，例如 `Qmc6309Driver`、`I2sAudioOutput`、`GpioHapticActuator`；
- 板级可枚举接口使用明确的 `<Role>Peripheral`，例如 `GpioPeripheral`、`SensorPeripheral`；
- 局部路由值使用 `PeripheralChannelId` 或更具体的 `GpioLine`、`SensorChannel`；
- 板级控制和呈现直接使用角色名，例如 `PowerController`、`BrightnessControl`、`DisplayTransition`；
- 每项 Guest 能力最多只有一个 `<Role>Service`；
- ABI 分派对象使用 `<Role>Endpoint`，不重复写 `ServiceEndpoint`；
- `Backend` 和 `Provider` 不用于本架构的类型名；第三方 API 中的既有术语不强行改写；
- `Unavailable` 可以描述缺失实现，但不附加 `Backend`，例如 `UnavailableGraphics`。

## 5. 示例

```text
ESP-IDF GPIO Driver
    ↓
Metalio-Claw4 GpioPeripheral
    ├─ channel 5  / "P5"
    └─ channel 15 / "P15"
    ↓
DeviceRegistry
    ↓
GPIO DeviceId
    ↓
GpioService
    ↓
GpioEndpoint
```

```text
ES8311 Driver + I2S Driver
    ↓
MosaicoAudioOutput
    ↓
AudioEngine
    ↓
AudioService
    ↓
AudioEndpoint
```

```text
PPA / DMA2D Driver
    ↓
DisplayTransition                  # Presentation，不进入 DeviceRegistry
    ↓
SquareSystemUi
```
