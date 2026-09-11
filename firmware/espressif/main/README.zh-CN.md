# Host 源码导航

`FirmwareApp` 是组合根，依赖方向为 `Runtime → Device contracts ← Platform`。

| 目录 | 职责 |
|---|---|
| `runtime/` | Session、Service、Bundle 与 Guest 资源 |
| `device/` | 硬件无关契约与设备身份 |
| `platform/boards/` | 引脚、器件组合、供电和初始化 |
| `platform/drivers/`、`platform/buses/` | 器件驱动与共享总线 |
| `platform/graphics/`、`platform/lvgl/` | 合成、呈现与 LVGL 适配 |
| `host/controller/` | 应用切换、电源与本地/远程控制 |
| `host/ui/` | 大厅、系统页面、手势与布局 |
| `work/` | 后台执行器与任务策略 |

[架构](../../../docs/design/architecture.zh-CN.md) · [构建](../../../docs/development/flashing.zh-CN.md)
