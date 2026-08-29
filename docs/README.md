# Documentation

`docs/` 只保留跨模块且需要长期维护的文档：

- [架构与发布基线](design/architecture.zh-CN.md)：产品边界、Host 分层、Guest–Host 数据路径、
  所有权、安全规则和尚未完成的发布门槛；
- [BundleFS 持久化格式](design/bundlefs.zh-CN.md)：v2 Catalog、离散数据块、写时复制和 v1 迁移；
- [Firmware 硬件分层与命名](design/firmware-terminology.zh-CN.md)：Board、Driver、Peripheral、Device、
  Service 和 ABI Endpoint 的固定词义；
- [C/C++ 代码风格](development/code-style.zh-CN.md)：Firmware、Guest SDK 和 C ABI 的编码规则与检查入口；
- [游戏音频规范](development/game-audio.zh-CN.md)：当前 App 的合成音效格式、感知约束和构建门禁；
- [定时器与大厅空闲功耗](development/timers-and-idle-power.zh-CN.md)：LVGL、`esp_timer`、周期唤醒和事件化边界；
- [Graphics 性能诊断](development/graphics-performance.zh-CN.md)：Scene、compositor 和显示链路的分段测量方法；
- [Host 构建与烧录指南](development/flashing.zh-CN.md)：P4 产品与 S31 preview 的环境、设备识别、烧录和排错；
- [USB 本地控制协议](design/usb-local-control.zh-CN.md)：CLI 本地 App 管理、日志、截图、输入和安全边界；
- [音效清单模板](development/game-sfx.template.json)：新游戏的起始配置。

与代码一起演进的组件级说明放在源码附近：

- [Guest C++ SDK](../guest/sdk/README.md)；
- [Guest–Host ABI](../guest/abi/README.md)；
- [Guest 构建、Bundle 与应用](../guest/README.md)；
- [Firmware 构建与烧录](../firmware/espressif/README.md)；
- [Firmware 源码边界](../firmware/espressif/main/README.md)。

## 文档维护规则

- 代码、ABI header 和可执行测试是行为的最终事实来源；
- 设计文档只描述当前决策和尚未完成的产品门槛，不保留被取代的候选 API；
- 一次性原型、原始串口/trace、不可复现的性能快照和历史里程碑不进入长期文档；可重复测量的聚合
  baseline 必须同时记录场景、硬件、构建模式和测量边界；
- 需要持续验证的行为写成 `guest/tests/conformance/`、Host test 或构建门禁；
- 项目尚未发布，未公开的旧名称和 wire 不保留 deprecated alias。

## 硬件来源

仓库不分发第三方原理图、数据手册或截图。Metalio-Claw4 的公开板级代码和说明见
[CloudZao/MetalioClaw4](https://github.com/CloudZao/MetalioClaw4)；ESP-Mosaico 和板载器件资料应从
Espressif 官方支持渠道取得。其他板卡和器件信息同样只引用厂商正式来源。

新的硬件引用只记录官方来源、型号、版本和必要的校验值；除非再分发许可明确允许，
不把第三方二进制文档纳入版本控制。
