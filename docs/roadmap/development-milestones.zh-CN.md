# MicroPixel 首版实施范围

首版目标平台是 ESP32-P4 / Metalio-Claw4。当前工作的目标不是继续维护历史阶段编号，而是让
Host、Demo 与 Snake 形成一套可发布、可重复构建的最小系统。

## 首版交付物

- ESP-IDF Host：WAMR AOT 加载、Bundle、Guest 隔离和设备 Service；
- Guest SDK：Clock、Timer、Input、Random、Storage、Resource、Graphics 与 Audio；
- Demo App：在同一 UI 中导航并手工测试各项 SDK 能力；
- Snake App：完整游戏、最终素材、Host 原生音频格式和持久化；
- P4 conformance：只保留 Runtime/SDK 边界、错误路径和基础设备协议测试。

## 发布检查

```sh
bash tools/build_guest_p4.sh
bash tools/build_demo_bundle.sh
bash tools/build_snake_bundle.sh
bash tools/build_p4_baseline.sh
```

在真机上至少确认：

- Host 启动后能加载并运行 Demo 与 Snake Bundle；
- Demo 的输入、随机数、存储、资源、Graphics 和 Audio 页面可操作；
- Snake 可开始、暂停、重新开始，触摸反馈和半透明贴图正常；
- 设备重启后随机序列不会固定，且启动阶段不运行 PSRAM 全量测试；
- 当前 P4 / Metalio-Claw4 Mode 1 音频硬件链路固定使用 16 kHz 采样率，播放状态能在实际结束时恢复。

## 首版不包含

- ESP32-S3 产品适配与回归矩阵；
- 历史 Stage C/Stage D Snake 验收脚本和静态报告；
- 独立 benchmark Guest、长期 soak 与编译失败样例库；
- 尚未被两个应用共同验证的 gamekit 抽象。

新的自动化测试应围绕当时仍存在的产品接口重新编写，不恢复旧 milestone contract。
