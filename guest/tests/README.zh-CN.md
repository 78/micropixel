# Guest conformance tests

这里保留仍由 `tools/build_guest_p4.sh` 构建的 ABI/SDK 回归用例，包括成功路径、非法 import/指针、
watchdog、退出语义、Service、Event、Graphics、Input、Audio 和 no-WASI STL 边界。

```sh
bash tools/build_guest_p4.sh
```

生成的 Wasm/AOT 位于 `build/guest-p4/`，不进入版本控制。`event_wait`、`touch_pressure` 和
`run_handler_*` 在真机运行时需要 Host 的合成事件钩子；使用
`firmware/espressif/sdkconfig.p4-conformance.defaults` 构建专用固件。
其余产品构建继续使用 `sdkconfig.p4.defaults`，不会链接这些钩子。

`linear_memory_limit` 是产品策略真机回归：默认 Host 必须拒绝 9 MiB 的 Guest 动态分配，同时允许
7 MiB 分配并访问首尾字节。它验证的是当前 8 MiB linear-memory 上限，因此不作为脱离 Host 的 Wasm
执行用例。

新增用例必须有明确断言并进入上述构建入口；仅用于一次测量的实验程序和原始数据不放在这里。
