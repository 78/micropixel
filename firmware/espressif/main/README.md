# MicroPixel Firmware 源码导航

`main/` 只包含 MicroPixel 自维护的 ESP-IDF Host 代码。第三方 WAMR、LVGL 与 ESP-IDF component 不在此目录内。

## 依赖方向

```text
                         ┌─ owns/initializes ─ Platform
FirmwareApp (组合根) ────┤                       │ implements
                         └─ creates ─ DeviceServices ── uses ── Device contracts
                                              ▲                    ▲
                                              │ uses               │
                                           Runtime                 └─ Platform
```

`device/` 和 `platform/` 在文件系统中同层，但职责不是平行重复：

- `device/` 定义与硬件无关的 Graphics、Input、Audio、Random 能力契约和 Runtime 使用的 façade；
- `platform/` 实现这些契约，并持有开发板、驱动、LVGL 与外设生命周期；
- `runtime/` 只依赖 `device/`，不得 include `platform/`；
- `FirmwareApp` 是唯一知道具体 `Platform` 并把 backend 注入 `DeviceServices` 的组合根。

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
├── firmware_app.cpp
├── firmware_app.hpp
├── conformance/
│   ├── CMakeLists.txt
│   ├── guest_test_hooks.cpp
│   └── guest_test_hooks.hpp
├── device/
│   ├── CMakeLists.txt
│   ├── audio.hpp
│   ├── graphics.hpp
│   ├── input.hpp
│   ├── random.hpp
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
│   │   ├── platform.cpp
│   │   ├── board_hardware.cpp
│   │   ├── board_hardware.hpp
│   │   ├── audio/
│   │   │   └── synth_audio.cpp
│   │   ├── input/
│   │   │   ├── gt911_input.cpp
│   │   │   └── gt911_input.hpp
│   │   └── display/
│   │       ├── dirty_region_coalescer.cpp
│   │       ├── dirty_region_coalescer.hpp
│   │       ├── esp_lcd_nv3051f.c
│   │       ├── esp_lcd_nv3051f.h
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
    ├── engine.cpp
    ├── engine.hpp
    ├── event_queue.cpp
    ├── event_queue.hpp
    ├── guest_context.cpp
    ├── guest_context.hpp
    ├── runtime_limits.hpp
    ├── touch_event_bridge.cpp
    ├── touch_event_bridge.hpp
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
    ├── resources/
    │   ├── bitmap_decoder.cpp
    │   ├── bitmap_decoder.hpp
    │   ├── bitmap_store.cpp
    │   ├── bitmap_store.hpp
    │   ├── resource_service.cpp
    │   └── resource_service.hpp
    ├── services/
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
- `runtime/bundle/` 负责 Bundle v2 格式、只读映射、校验和 AOT payload 所有权。v2 使用显式长度的 64 字节 AppId，并将 Header 固定为 128 字节；对外格式由 `bundle_format.h` 固定，目录调整不改变磁盘 ABI。
- `runtime/resources/` 负责异步资源请求、图片解码和 Bitmap handle/PSRAM 配额。
- `runtime/services/` 放 Runtime 自己提供的 Timer、Storage 业务；它们不是物理设备 backend。
- `runtime/wamr/` 负责 WAMR 初始化、module/instance/exec-env RAII、watchdog 和运行期诊断。
- `platform/graphics/` 是跨板级图形协议校验；`platform/metalio-claw4/` 只放该开发板的实现，并按真实硬件子系统分为 `display/`、`input/`、`audio/`。
- `platform/null/` 提供没有真实板级设备时的构建实现。

只在 `device/`、`runtime/`、`platform/`、`conformance/` 这四个真实子系统设置 CMake 清单；不为 `abi/` 等叶子目录继续增加小型 CMake 文件。
