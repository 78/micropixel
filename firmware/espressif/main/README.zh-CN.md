# Host 源码导航

`FirmwareApp` 是组合根，依赖方向为 `Runtime → Device contracts ← Platform`。

| 目录 | 职责 |
|---|---|
| `runtime/` | Session、Service、Bundle 与 Guest 资源 |
| `device/` | 硬件无关契约与设备身份 |
| `platform/boards/` | 引脚、器件组合、供电和初始化 |
| `platform/drivers/`、`platform/buses/` | 器件驱动与共享总线 |
| `platform/graphics/`、`platform/lvgl/` | 合成、呈现与 LVGL 适配 |
| `platform/storage/` | `device::BlockStorage` 介质适配器（NOR 分区、SPI NAND） |
| `platform/memory/` | PSRAM 分配器、`MICROPIXEL_EXT_RAM_BSS`、`PsramString` / `PsramVector` / `PsramMap` 与 `PsramBuffer` |
| `runtime/bundlefs/`、`runtime/bundle/` | BundleFS 实例、Bundle source 与多商店 `AppStore` |
| `host/controller/` | 应用切换、电源与本地/远程控制 |
| `host/ui/` | 大厅、系统页面、手势与布局 |
| `work/` | 后台执行器与任务策略 |

大厅安装展示由 `host/ui/hall_install_model.hpp` 组织：新 App 的进度卡置于首位，更新已有 App 时保留原位。
进度更新按 App ID 查找展示位置，卡片操作按 App ID 映射回实际目录索引；不改变存储目录。

[架构](../../../docs/design/architecture.zh-CN.md) · [构建](../../../docs/development/flashing.zh-CN.md)
