# MicroPixel Espressif firmware

这是 MicroPixel 的 Espressif Host 固件工程，使用 ESP-IDF 6.1 和固定 commit 的 WAMR fork。当前
product profile 是 ESP32-P4 + Metalio-Claw4，preview bring-up profile 是 ESP32-S31 + ESP-Mosaico；
两者复用 Runtime、Device contracts、Guest ABI 和方屏 Host UI。先按仓库根目录 README 初始化 submodule，
并设置 `IDF_PATH`、WASI Clang 和 `WAMRC`。

日常增量构建、Host 烧录、显式 fullclean、空 App Store 重置和示例 App 安装见
[Host 构建与烧录指南](../../docs/development/flashing.zh-CN.md)。统一入口不会把这些行为混在一起：

```sh
bash tools/p4.sh build-host
bash tools/p4.sh flash-host /dev/cu.usbmodemPORT  # 保留 App Store
bash tools/p4.sh monitor /dev/cu.usbmodemPORT     # 仅监控，不构建或烧录
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT  # 清空并写入七个示例 App
bash tools/p4.sh flash-all /dev/cu.usbmodemPORT   # Host 和七个示例 App，不跑测试
bash tools/p4.sh test                              # 仅发布前或推送前运行
bash tools/p4.sh --help
```

ABI/SDK conformance 的完整 Guest 基线仍可单独运行：

```sh
bash tools/build_guest_p4.sh
```

Host 输出到 `build/host-esp32p4/`，conformance Guest 输出到 `build/guest-p4/`。两者均为本地产物。

ESP-Mosaico 使用独立的、带 ESP32-S31 芯片核验的 preview 入口；Null profile 只做依赖方向编译，不能烧录：

```sh
bash tools/s31.sh build-host
bash tools/s31.sh flash-host /dev/cu.usbmodemPORT
bash tools/s31.sh monitor /dev/cu.usbmodemPORT
bash tools/s31.sh build-null
```

## 构建配置

- `sdkconfig.p4.defaults`：产品 defaults，使用固定 task core、dirty-region coalescing，并关闭测试钩子；
- `sdkconfig.p4-conformance.defaults`：仅为 `event_wait`、`touch_pressure` 和 `run_handler_*` 加入合成 Host 事件；
- `sdkconfig.p4-null.defaults`：覆盖产品 defaults，编译 Null Platform 以检查 Runtime/Device 的硬件无关路径；
- `sdkconfig.s31.defaults`：ESP-Mosaico preview defaults；`sdkconfig.s31-null.defaults` 只选择 S31 Null Board；
- `sdkconfig.debug.defaults`：显式追加时启用的调试配置。

需要专用 conformance 固件时，可把两个 defaults 一起传给现有构建脚本：

```sh
P4_SDKCONFIG_DEFAULTS="$PWD/firmware/espressif/sdkconfig.p4.defaults;$PWD/firmware/espressif/sdkconfig.p4-conformance.defaults" \
    bash tools/p4.sh build-host
```

P4 产品固件常驻 USB Serial/JTAG 开发通道，可以通过统一 CLI 获取最终的 LVGL 合成画面：

```sh
python3 tools/micropixel --transport usb --port /dev/cu.usbmodemPORT \
  screenshot --output build/captures/current.jpg
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
