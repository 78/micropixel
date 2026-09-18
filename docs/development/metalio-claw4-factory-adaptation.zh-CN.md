# Metalio-Claw4 原厂适配

## 驱动依赖与生命周期

P4 从组件仓库固定使用 `78/uart-uhci 0.4.0`、`78/uart-eth-modem 0.7.0` 和 `espressif/iot_eth 1.1.0`，
不维护本地驱动副本。模组 TX 使用 32 深度的固定池；TX 与重组缓冲显式使用 PSRAM，分配失败不回退 SRAM。
AT 命令沿用上游字符串接口。
本地诊断可启用 `CONFIG_MICROPIXEL_CELLULAR_DEBUG`，使用上游 `SetDebug` 输出 AT、UART 帧及握手；
默认关闭，不改变全局日志级别。日志影响时序，且可能含模组/SIM 标识，原始采集不提交到仓库。
Host 网络维护定时器每 5 秒请求信号采样，读数或连接变化通过统一网络事件更新图标；页面和 Guest 运行不影响采样。

`Stop()` 超时后仍保留模组对象及供电，阻止 SIM、开关和 OTA 操作，并在既有 BackgroundExecutor
重试停止。恢复任务每次等待停止至多 100 毫秒，失败后延迟 100 毫秒并排到队尾；队列满时由 Host 网络维护定时器
补交。停止成功后才断电，并按原启用状态恢复。休眠失败不会永久遗留 paused 状态。
模组异常重启通过同一任务停止后重新启动，初始化失败只清理；缺卡保留 AT 通道，掉注册交由上游持续入网控制。
整机重启与关机终止整个固件，不在驱动回调内析构或同步停止模组。

## SIM 控制

对照原厂 `ca3aa3fa027ff7dad2adf0c2d03c4f24aa838950` 的
`main/display/screen/network_screen/network_screen.cc` 中 `sim_switch_task` 与
`main/boards/common/nt26_board.cc` 中 `SendAtCommand`。

Host 契约为 [Cellular](../../firmware/espressif/main/device/contracts/cellular.hpp)，
板级实现为 [CellularController](../../firmware/espressif/main/platform/boards/metalio-claw4/cellular_controller.cpp)。

- `RequestSimRefresh()` 在既有 BackgroundExecutor 查询 `AT+ECSIMCFG?`，槽位 0 为外置卡、1 为内置卡。
  查询失败或非法响应返回未知状态，不默认显示某张卡。已知当前槽位的重复选择不发送命令，也不重启。
- `SetSimSlot()` 仅在 4G 模式接受内置或外置槽位。无需联网或初始化完成，缺卡时仍可尝试另一张卡。
- 切换前调用 `PrepareForShutdown()` 暂停自动 PDP/注册控制，同时保留 AT 通道。准备失败不发送射频或槽位命令，
  通过停止/重启恢复控制；槽位切换失败也重建驱动，解除数据激活抑制。
- 切换遵循原厂命令顺序和超时：`CFUN=0`（8 秒）、等待 500 毫秒、`ECSIMCFG=SimSlot,X`
  （5 秒）、等待 500 毫秒、`CFUN=1`（15 秒）。首步失败不再写槽位；槽位写入失败仍用
  `CFUN=1`（10 秒超时）尽力恢复射频。与原厂一致，最后一步失败不撤销已成功写入的槽位。
- 成败都回读模组，模组持久化实际槽位。当前接口不维护第二份 NVS 槽位偏好。
- SIM 操作与网络模式切换通过既有操作锁串行。关闭 4G 可取消排队中的 SIM 请求；休眠或关机前排队、尚未执行
  的 SIM 请求取消；即使先恢复、再执行旧请求，也保持取消。取消任务未出队前不接受新 SIM 请求。
- 停止模组后若断电 I²C 操作失败，拒绝休眠并尝试恢复 4G；恢复失败发布失败状态。关机路径不重新上电。

系统设置的 `4G / SIM` 项和状态层的 4G 卡片共用网络设置，自动读取槽位、选择另一张 SIM
或独立开关 4G，Wi-Fi 仍可同时启用。系统设置入口仅在板型提供蜂窝能力时显示，返回键回到原来的设置列表。
SIM 操作结束后停止驱动，将模组复位/供电线拉低至少 100 毫秒，再启动驱动。
只重建移动网络连接，不调用整机 `esp_restart()`；Wi-Fi 和应用会话保留。当前槽位按钮不可重复选择。
其他原厂功能仍需逐项对照，不能用本项测试证明整板适配完成。

## 网络诊断页面

4G / SIM 页复用 Wi-Fi 页的全屏标题栏、卡片、滚动列和板型密度配置。连接状态显示在顶部 4G 开关行中，
不设独立状态卡片或手动刷新按钮。关闭时隐藏其下的 SIM 选择、网络详情及说明，启用后再显示。
SIM 选项标明当前槽位；SIM 切换提示移动网络连接会重启。详情包含 SIM 就绪/PIN/PUK/缺卡、CSQ 信号、
注册状态、运营商、射频、数据附着、APN 与 PDP 地址。PDP 地址是模组读数，不等同于 Host 已联网。

打开页面或网络状态变化时，既有 BackgroundExecutor 在操作锁内查询槽位及 `CPIN?`、`CFUN?`、`CSQ`、
`CEREG?`、`CGATT?`、`COPS?`、`CGDCONT?` 和 `CGPADDR=1`，无需模组先注册成功。
各诊断命令超时 1 秒，槽位查询沿用 5 秒；查询只读，不设置 APN、不切换运营商。
结果存入固定容量 Host 契约，格式错误、超长字段和失败查询显示未知，不沿用旧成功值。
初始化、连接成功、缺卡及注册状态变化会提交后台采样，页面无需退出重进；
仅在 AT 通道就绪后提交，队列繁忙或初始化未结束时由网络维护定时器补交。
自动诊断查询不禁用 4G 开关；关闭请求提交成功后取消剩余查询，待当前 AT 命令结束后停止模组。
休眠清除旧读数。详情行使用内容撑高和对称内边距，换行内容保持在分隔线以内。
`CSQ=99` 显示信号未知，搜网状态建议检查天线和覆盖，不推断 SIM 是否激活。


## OTA 与网络配置互斥

OTA 下载前通过 Host `Network::TryBeginFirmwareUpdate()` 原子占用网络配置。Wi-Fi 的用户配置
调用与取得保留串行；Wi-Fi 连接建立中、蜂窝启停/切卡/恢复未完成时拒绝保留。
蜂窝 `TryHoldConfiguration()` 只负责设备配置与休眠保护，不依赖 OTA 业务。只读 SIM 诊断可继续，
不阻止保留。保留期间 Wi-Fi 和 4G 的开关、连接、遗忘及切卡操作统一返回忙碌。
本地更新请求和远端更新命令共用该入口，由 RAII 释放占用。自动掉线/重连仍可能发生，传输层负责报告失败。
没有蜂窝硬件时，Host 仍保护 Wi-Fi 用户配置，不需要特殊 OTA 路径。

## 验证

执行 `bash tools/tests/test_firmware_host.sh`，其中
[test_cellular_controller.cpp](../../tools/tests/test_cellular_controller.cpp) 覆盖命令顺序、超时、缺卡、
非法响应、同槽位无操作、失败恢复、请求互斥、队列拒绝、跨休眠恢复取消、诊断排队/执行中关闭、AT 未就绪时关闭以及断电失败回滚。
OTA 测试还覆盖两个方向的互斥、释放后恢复操作、Wi-Fi 模式更新，以及 64 轮真实线程竞争。
诊断测试覆盖未注册读取、缺卡错误、未知 CSQ、注册拒绝、带逗号的运营商名称、CID 匹配、
超长和损坏响应、失败查询清除旧值，以及 PIN/PUK 和已注册但未附着的提示。
停止超时测试覆盖保持供电、拒绝新操作、重试恢复，以及模组重启、初始化失败和 SIM 准备失败。
再执行 `bash tools/check_firmware_style.sh --format-only` 和 `bash tools/p4.sh build-host`。
UHCI RX lease、TX 固定池与底层 Stop 协议由上游组件的 host tests 覆盖。

尚需真机确认：两种 SIM 实际入网、未插外置卡时切换内置卡、模组重启后槽位保持、
射频恢复超时后的搜网、休眠恢复与关机行为。Host stub 不替代这些验收。
