# Guest apps

Apps use the C++23 SDK and Service ABI; they do not depend on ESP-IDF, LVGL, or board types.
A single-threaded event loop handles input, timers, and lifecycle events.

| Directory | Contents |
|---|---|
| [sdk](sdk/README.md) | Public C++ API |
| [abi](abi/README.zh-CN.md) | Guest–Host protocol |
| `runtime/` | SDK-to-wire conversion |
| `apps/` | Example apps |
| [tests](tests/README.zh-CN.md) | Conformance tests |

## Run an example

With the SDK toolchain configured and a MicroPixel device connected:

```sh
python3 tools/micropixel --transport usb run guest/apps/sdk-demo
```

For a release Bundle, choose the architecture explicitly:

```sh
python3 tools/micropixel package guest/apps/sdk-demo --aot-target riscv32-ilp32f
```

P4/S31 use `riscv32-ilp32f`; S3 uses `xtensa`. Both require the matching MicroPixel WAMRC and AOT v6.

[Quickstart](sdk/QUICKSTART.md) · [Publishing](sdk/PUBLISHING.md) · [Build and Bundle reference (中文)](README.zh-CN.md)
