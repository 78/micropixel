# Firmware

`firmware/<vendor>/` 存放可独立编译、生成设备映像并烧录的 Host 工程。当前 Espressif 工程包含：

```text
firmware/
└── espressif/   # ESP32-P4 产品 + ESP32-S31 preview，使用 ESP-IDF 6.1
```

厂商 SDK 是外部构建依赖，不纳入仓库。只有同一厂商确实需要独立的 product、factory-test 或 recovery
映像时，才在厂商目录下增加映像层级。新增平台应复用 Guest ABI 和 Runtime service 边界，只在对应
vendor 工程中实现启动、Device backend、board profile 与打包入口。

具体边界见 [MicroPixel 架构与发布基线](../docs/design/architecture.zh-CN.md)。
