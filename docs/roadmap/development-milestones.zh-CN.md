# MicroPixel 首版实施范围

首版目标平台是 ESP32-P4 / Metalio-Claw4。当前工作的目标不是继续维护历史阶段编号，而是让
Host、Demo 与 Snake 形成一套可发布、可重复构建的最小系统。

## 首版交付物

- ESP-IDF Host：长驻 WAMR、单 AppSession、Bundle、Guest 隔离、设备 Service 与 Host 原生 System Shell；
- Guest SDK：Clock、Timer、Input、Random、Storage、Resource、Graphics 与 Audio；
- Demo App：在同一 UI 中导航并手工测试各项 SDK 能力；
- Snake App：完整游戏、最终素材、Host 原生音频格式和持久化；
- P4 conformance：只保留 Runtime/SDK 边界、错误路径和基础设备协议测试。

## System Shell 实施顺序

产品允许安装多个 App，但任何时刻最多保留一个 Guest AppSession。按以下顺序推进：

1. **基础生命周期边界（已实现）**：一次性 Engine 拆为长驻 `AppRuntime` 和单次 `AppSession`；Guest
   正常退出、Trap 或启动失败后完整释放 Session，不再重启设备，转入 Host 原生 App Hall 骨架；
2. **暂停与系统手势（已实现，待真机回归）**：实现 `Foreground ↔ Suspended`、顶部下滑全屏 Status Layer、底部上滑截图缩回大厅，
   并保证系统手势不泄漏给 Guest；
3. **App 目录与切换（已实现，待真机回归）**：扫描多个 Bundle，未运行卡片读取 Flash 封面，唯一挂起卡片使用 Host 最后一帧；
   点击其他 App 时先销毁旧 Session，再启动新 Session；
4. **大厅管理能力（部分实现）**：亮度 PWM、Host 主音量和 FPS 开关已接入；安装/卸载、网络配置、
   Wi-Fi/4G 实际服务和错误恢复待实现；
5. **性能蒙层（已实现，待真机回归）**：由 Host 在最终显示画面叠加实际呈现 FPS 与整机聚合 CPU 使用率，
   不把绘制职责交给 Guest。

这一阶段不新增 Guest Lifecycle callbacks；Guest 启动沿用 `main()` / `__micropixel_start`，暂停和恢复对 Guest
透明。只有出现明确的 App 侧需求时才扩展 typed event。

## 发布检查

```sh
bash tools/build_guest_p4.sh
bash tools/build_p4_baseline.sh
bash tools/build_system_shell_p4.sh
bash tools/tests/test_firmware_host.sh
python3 -m unittest tools.tests.test_build_app_store_image
bash tools/tests/test_bundle_reader.sh build/system-shell-p4/app-store.bin
```

在真机上至少确认：

- Host 启动后停留在大厅，并在一屏显示 Blocks、Snake 与 Demo 的 Flash 封面；
- 三张卡片都能启动对应 Guest，点击另一个 App 时旧 Session 已先销毁；
- 底部上滑能暂停并截图回大厅，运行卡片能恢复同一个 Session；顶部下滑能暂停并打开完整状态层；
- 状态层亮度、音量和 FPS 开关可操作，恢复 Guest 后 FPS/CPU 小蒙层由 Host 显示；
- Snake 可开始、暂停、重新开始，触摸反馈和半透明贴图正常；
- 设备重启后随机序列不会固定，且启动阶段不运行 PSRAM 全量测试；
- 当前 P4 / Metalio-Claw4 Mode 1 音频硬件链路固定使用 16 kHz 采样率，播放状态能在实际结束时恢复。

## 首版不包含

- ESP32-S3 产品适配与回归矩阵；
- 历史 Stage C/Stage D Snake 验收脚本和静态报告；
- 独立 benchmark Guest、长期 soak 与编译失败样例库；
- 尚未被两个应用共同验证的 gamekit 抽象。
- System Shell 的在线安装、网络配置和过渡动画；这些已经排入上述后续顺序，不属于当前首个可交互版本的
  构建验收阻塞项。

新的自动化测试应围绕当时仍存在的产品接口重新编写，不恢复旧 milestone contract。
