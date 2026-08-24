# MicroPixel Espressif firmware

这是当前支持的 ESP32-P4 Host 固件工程，使用 ESP-IDF 6.1、固定 commit 的 WAMR fork 和
Metalio-Claw4 板级 backend。先按仓库根目录 README 初始化 submodule，并设置 `IDF_PATH`、WASI
Clang 和 `WAMRC`。

新设备、System Shell 和仅更新 Guest 的完整步骤见 [ESP32-P4 烧录指南](../../docs/development/flashing.zh-CN.md)。

```sh
bash tools/build_p4_baseline.sh
bash tools/flash_p4_baseline.sh /dev/cu.usbmodemPORT
bash tools/monitor_p4.sh /dev/cu.usbmodemPORT
```

也可以构建、烧录并等待 Hello Guest 成功标记：

```sh
bash tools/run_p4_baseline.sh /dev/cu.usbmodemPORT
```

Host 输出到 `build/host-esp32p4/`，conformance Guest 输出到 `build/guest-p4/`。两者均为本地产物。

## 构建配置

- `sdkconfig.p4.defaults`：产品 defaults，使用固定 task core、dirty-region coalescing，并关闭测试钩子；
- `sdkconfig.p4-conformance.defaults`：仅为 `event_wait`、`touch_pressure` 和 `run_handler_*` 加入合成 Host 事件；
- `sdkconfig.p4-null.defaults`：覆盖产品 defaults，编译 Null Platform 以检查 Runtime/Device 的硬件无关路径；
- `sdkconfig.debug.defaults`：显式追加时启用的调试配置。

需要专用 conformance 固件时，可把两个 defaults 一起传给现有构建脚本：

```sh
P4_SDKCONFIG_DEFAULTS="$PWD/firmware/espressif/sdkconfig.p4.defaults;$PWD/firmware/espressif/sdkconfig.p4-conformance.defaults" \
    bash tools/build_p4_baseline.sh
```

P4 产品固件常驻 USB Serial/JTAG 开发通道，可以直接获取最终的 720x720 LVGL 合成画面：

```sh
python3 tools/capture_p4_screen.py /dev/cu.usbmodemPORT build/captures/current.png
```

屏幕抓取和 USB 触摸注入不增加 Guest ABI；它们用于 App 作者调试 Host 最终合成结果。

## 源码边界

```text
FirmwareApp (composition root)
├── Platform ── implements ──> Device backend contracts
└── Runtime  ── uses ────────> DeviceServices
```

`device/` 与 `platform/` 虽然在目录树中同层，但前者是硬件无关契约，后者是板级实现；Runtime 只依赖
Device。完整到每一个文件的源码树、目录职责和依赖规则见 [`main/README.md`](main/README.md)。

Guest ABI 和 SDK 不依赖 ESP-IDF、LVGL 或具体开发板。`components/wasm-micro-runtime/` 是指向
`78/wasm-micro-runtime` 的 Git submodule，固定在 `wamr-host/esp-idf-psram` 分支的已验证 commit。
WAMR 修改应先提交到该 fork，再在本仓库更新 gitlink；不要把 WAMR 源码复制回主仓库。
