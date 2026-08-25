# MicroPixel 架构与发布基线

本文只记录当前产品架构、稳定边界和尚未完成的发布门槛。已被代码取代的原型、候选 API、
历史里程碑和一次性性能实验不在仓库中继续维护。

具体公开接口以 [Guest C++ SDK](../../guest/sdk/README.md)为准，机器可读协议以
[`micropixel_abi.h`](../../guest/abi/micropixel_abi.h) 和 [Guest–Host ABI](../../guest/abi/README.md)为准，
Firmware 文件职责以 [Espressif main README](../../firmware/espressif/main/README.md)为准。

## 1. 产品边界

MicroPixel 是面向嵌入式设备的 WebAssembly 应用运行时。当前产品基线为：

- Host：ESP32-P4 + Metalio-Claw4，ESP-IDF 6.1；
- Runtime：MicroPixel WAMR fork 固定 commit、AOT format v6，同时最多运行一个 Guest `AppSession`；
- Guest：受限 C++23 profile，不直接依赖 ESP-IDF、LVGL 或板级 SDK；
- App 分发：Bundle v1 封装 AOT、资源和 App metadata；24 MiB 可写 App Store 由 BundleFS 以离散
  64 KiB 块和四 Bank Catalog 提供写时复制事务；
- 系统 UI：Host 原生 App Hall、Status Layer 和系统手势，不作为 Guest App 运行。

当前不承诺 ESP32-S3 产品适配、多 Guest 并行、Guest 多线程、生产级在线 App 分发、网络 Service、
Camera Service 或通用 Widget Server。Remote Control 的开发版在线安装已经进入代码基线，但 package
数字签名、BundleFS 真机断电恢复矩阵和完整 TLS peer 验证仍是发布门槛。BundleFS 的块号表允许物理块
离散分布，卸载后的块可直接复用，不依赖连续 extent GC。

## 2. Host 分层

```text
app_main
    └── FirmwareApp                         # 唯一组合根
        ├── Platform
        │   ├── Metalio-Claw4                # display/input/audio/system UI
        │   └── Null                          # 硬件无关编译基线
        ├── DeviceServices
        │   ├── Graphics / Input / Audio
        │   └── Random
        ├── AppRuntime                            # 长驻 WAMR
        │   └── AppSession (0..1)
        │       ├── Bundle / module / instance / exec env
        │       ├── GuestContext / Service endpoints
        │       ├── EventQueue / Timer / Storage / Resource
        │       └── ABI adapter
        ├── HostController / SystemShell
        │   ├── App Hall
        │   ├── Foreground / Suspended
        │   └── Status Layer / System gestures
        └── Conformance hooks                    # 仅测试配置
```

依赖方向固定为：

```text
Runtime -> Device contracts <- Platform
```

- `device/` 定义硬件无关能力契约；
- `platform/` 实现板级 backend，不 include Runtime；
- Runtime 只使用注入的 `DeviceServices`，不 include Platform；
- `FirmwareApp` 是唯一同时知道 Platform、Device 和 Runtime 的组合根；
- `runtime/abi/` 只做 Guest 内存验证、wire 转换、上下文查找和转发，不承担业务规则。

## 3. Session 与事件模型

`AppRuntime` 与 Firmware 同寿命，WAMR 只初始化一次。每次启动 Guest 创建一个 `AppSession`，
并按 Bundle mapping → module → instance → exec env → `GuestContext` 的所有权链管理。正常返回、Trap、
启动失败和应用切换使用同一套逆序清理。

Guest 为单线程事件模型，标准入口只有：

```cpp
micropixel::Application app;
micropixel::Timer timer = app.timers().Every(16_ms);

app.Run([&](const micropixel::Event& event) {
    if (const micropixel::TimerEvent* tick = event.TimerFrom(timer)) {
        Update(*tick);
        Render();
    } else if (const micropixel::TouchEvent* touch = event.touch()) {
        HandleTouch(*touch);
    } else if (event.type() == micropixel::EventType::kResume) {
        Render();
    } else if (event.type() == micropixel::EventType::kStop) {
        SaveState();
    }
});
```

- handler 串行执行，不会在一个 handler 中间派发下一个事件；
- `Stop` 先交给 handler，handler 返回后 `Run()` 返回；
- Timer 只由 `app.timers().After/Every()` 创建，并作为普通 Event 匹配；
- `WaitEvent()` 只保留给有限等待和协议测试，不是长期 App 的第二套标准写法；
- Host 暂停 Guest 时同时暂停 watchdog；恢复同一 Session 时投递 `Resume`；
- 系统手势由 Host 拦截，不泄漏给 Guest。

## 4. Guest–Host 边界

Public C++ SDK 不直接暴露 C ABI。`guest/runtime/sdk.cpp` 将强类型对象 lower 到七个 Core imports：

1. `abi_version`
2. `log_write`
3. `clock_now`
4. `event_wait`
5. `service_open`
6. `service_call`
7. `service_submit`

能力通过带独立 major/minor 的 Service 演进：

| Service | ID | 当前传输 |
|---|---:|---|
| Timer | 1 | call + event |
| Storage | 2 | call |
| Resource | 3 | call |
| Random | 4 | call |
| Graphics | 16 | call + submit |
| Input | 17 | call + event |
| Audio | 18 | call |
| Network | 19 | 仅预留，未实现 |

三条数据路径的职责不混用：

- `service_call`：低频、有界的控制请求和响应；
- `service_submit`：Renderer Frame command stream 等高频或批量数据；
- `event_wait`：Timer、Touch、Resume 和 Stop 等异步通知。

`micropixel_event_t` 固定为 48 bytes，按 `service_id + event_id` 解码。Service descriptor、handle 和
设备信息在 Guest 生命周期内缓存；Host 热路径使用固定容量 Registry，不做字符串查找或堆分配。

版本兼容规则为：Service major 必须相同，Host minor 必须不低于 Guest 的最低要求。已发布的
Service、method、channel、event、capability 和 opcode ID 不得改义或复用。新能力优先增加
method/channel/event，其次增加 Service，只有真机证据证明现有传输不足时才增加 Core import。

## 5. Graphics 与 Resource

公开 Graphics 使用 SDL3 风格概念：

```text
Renderer -> Frame -> Present
Resources -> Texture
Renderer -> StreamingTexture / TextureUpdateBatch
```

- `Frame` 直接提供 `Clear`、`FillRect`、`DrawText`、`DrawTexture` 和
  `Save/SetClipRect/Translate/Restore`；
- 一个 Frame 是一次原子场景替换，`Present()` 显式发布；析构不会隐式 Present；
- SDK 将大 Frame 自动分成有界 batch，应用不感知 transport 或 retained 优化开关；
- 公开资源概念统一为 `Texture`；Host 内部的 `BitmapView` 只是 CPU 像素内存描述符，
  不是公开资源身份；
- `Resources::LoadTexture()` 同步返回 `Result<Texture>`，Host 可在内部使用 worker 解码；
- `Texture` 和 `StreamingTexture` 是 move-only RAII 对象，`Reset()` 立即令 Guest 句柄无效；
- retained scene 持有独立 Texture 引用。Guest `Reset()` 只撤销 Guest 引用，当 scene 引用也归零时
  才释放像素内存；
- `PixelFormat::kBgr888` 和 `kBgra8888` 按 Guest 内存中的字节顺序命名；
- `StreamingTexture::Update()` 按 dirty rect 更新，`TextureUpdateBatch` 合并 damage 并在
  `Finish()` 时统一唤醒 compositor。

## 6. 所有权、并发与错误

- Firmware 的 WAMR、Bundle mapping、NVS、timer、queue、task 和板级句柄使用 move-only RAII
  或 scope-bound binding 管理；
- 裸指针默认 non-owning；跨异步边界必须由 binding token、join 或 shutdown protocol
  证明生命周期；
- ISR 只记录最小 POD 状态并唤醒任务，不调用 WAMR、Guest 或 LVGL；
- 跨任务状态使用固定容量队列、数组和对象池，实时路径不隐式扩容；
- shutdown 顺序是停止接收、唤醒 worker、join，再释放队列和底层句柄；
- Guest 可恢复失败返回 `Result<T>`；无效句柄、SDK/Host 自相矛盾或 ABI 安全失败进入
  panic/fault policy；
- 析构函数只做 best-effort cleanup，不 Panic、不抛异常；
- exception 和 RTTI 保持关闭，Public SDK 不暴露 STL ABI、Host 指针或 C++ 对象布局。

## 7. Bundle、能力与权限

Bundle v1 当前使用 128-byte header 和 48-byte section，section 可包含 AOT、Asset 和 App
metadata。Bundle reader 在创建 WAMR instance 前完成 magic、hash、范围、对齐、格式和唯一性检查。
Bundle 文件由 [BundleFS](bundlefs.zh-CN.md) 保存；Catalog 位于 `app_store` 自身，不使用 NVS，也没有
预置 App 或连续 extent 的特殊语义。

设备能力与 App 权限必须分开建模：

- Service requirements 回答设备是否提供某 Service、版本和语义能力；
- Permission grant 回答当前 App 是否获准执行某个受保护操作；
- 缺少 Service 应报告 `NotFound/Unsupported/VersionMismatch`，未授权应报告
  `PermissionDenied`；
- `service_open` 只做 Service 发现和版本协商，不承担用户授权流程；
- 权限应按动作划分，例如 `storage.read` 和 `storage.write`，不用过宽的 Service 级布尔值。

当前 Bundle 尚没有 requirements section，权限声明和 grant 也未实现；这两项不得用
Guest 进入 `main()` 后的 Trap 代替。

## 8. 发布基线

当前集成应用为 Blocks、Snake 和 Demo。Demo 覆盖公开 Service，Snake 覆盖高频 Graphics、
Input、Storage 和 Audio，Blocks 覆盖 StreamingTexture 与批量 damage。

自动基线：

```sh
bash tools/build_guest_p4.sh
bash tools/p4.sh build-host
bash tools/p4.sh build-all
bash tools/check_firmware_style.sh
bash tools/tests/test_firmware_host.sh
python3 -m unittest tools.tests.test_build_app_store_image
```

真机发布前至少检查：

- App Hall 能显示并启动最多 50 个 App，第一行可自由左右拖动并带惯性、不强制卡片吸附；新安装 App 位于
  最左侧。卡片和解码封面使用最多 6 项的视口窗口，快速滑动期间允许占位；切换时旧 Session 已先销毁；
- App Hall 顶部居中显示紧凑 FPS/CPU，右侧 Wi-Fi 信号与电池状态不会互相遮挡；
- 顶部下滑打开 Status Layer，底部上滑暂停并返回大厅；
- 恢复同一 App 时复用 Session 和 retained scene，并由 PPA 从 Hall 卡片硬件放大回最后一帧；
- Texture `Present → Reset → 重绘/换场景` 不产生 UAF；
- Timer 积压时 `elapsed` 和 `missed_count` 正确；
- GT911 不声明 pressure capability，Touch 坐标与 Renderer 使用同一逻辑空间；
- 亮度、Host 主音量、FPS/CPU 蒙层、音频结束状态和设备重启后的随机源正常。

尚未完成的发布门槛只保留下列当前事项：

1. Bundle requirements 与 WAMR instance 创建前的兼容性 preflight；
2. 权限声明、grant 与 method 级检查；
3. Resource/Graphics/Input 协议版本冻结、兼容 fixture 和 wire 负向/fuzz 回归；
4. Texture retained 生命周期、Timer 积压、Run/Stop 和三个应用的真机回归；
5. System Shell 的在线安装/卸载、网络配置和完整错误恢复。

## 9. 架构禁止项

- 不引入深继承树、万能 Manager、Service Locator 或新的全局可变状态；
- 不让 `FirmwareApp`、ABI adapter 或 Platform 承担本属于领域 Service 的业务规则；
- 不在 C ABI 中暴露 C++ class、`std::expected`、vtable、STL 类型或 Host 指针；
- 不用裸 `new/delete`、无界容器或 detached task 承担实时资源与生命周期；
- 不为新板型分叉 Guest API；板级差异通过 Device contract、Service capability 和 Bundle
  requirements 表达；
- 不修改第三方源码以迎合本项目格式、命名或文档结构。
