# Metalio-Claw4 原厂适配

## SIM 控制

对照原厂 `ca3aa3fa027ff7dad2adf0c2d03c4f24aa838950` 的
`main/display/screen/network_screen/network_screen.cc` 中 `sim_switch_task` 与
`main/boards/common/nt26_board.cc` 中 `SendAtCommand`。

Host 契约为 [Cellular](../../firmware/espressif/main/device/contracts/cellular.hpp)，
板级实现为 [CellularController](../../firmware/espressif/main/platform/boards/metalio-claw4/cellular_controller.cpp)。

- `RequestSimRefresh()` 在既有 BackgroundExecutor 查询 `AT+ECSIMCFG?`，槽位 0 为外置卡、1 为内置卡。
  查询失败或非法响应返回未知状态，不默认显示某张卡。已知当前槽位的重复选择不发送命令，也不重启。
- `SetSimSlot()` 仅在 4G 模式接受内置或外置槽位。无需联网或初始化完成，缺卡时仍可尝试另一张卡。
- 切换遵循原厂命令顺序和超时：`CFUN=0`（8 秒）、等待 500 毫秒、`ECSIMCFG=SimSlot,X`
  （5 秒）、等待 500 毫秒、`CFUN=1`（15 秒）。首步失败不再写槽位；槽位写入失败仍用
  `CFUN=1`（10 秒超时）尽力恢复射频。与原厂一致，最后一步失败不撤销已成功写入的槽位。
- 成败都回读模组，模组持久化实际槽位。当前接口不维护第二份 NVS 槽位偏好。
- SIM 操作与网络模式切换互斥，并与休眠/恢复通过既有操作锁串行；休眠或关机前排队、尚未执行
  的 SIM 请求取消；即使先恢复、再执行旧请求，也保持取消。取消任务未出队前不接受新 SIM 请求。
- 停止模组后若断电 I²C 操作失败，拒绝休眠并尝试恢复 4G；恢复失败发布失败状态。关机路径不重新上电。

系统设置的 `4G / SIM` 项和状态层的 4G 卡片共用网络设置，可刷新槽位、选择另一张 SIM
或重启切换 Wi-Fi/4G。系统设置入口仅在板型提供蜂窝能力时显示，返回键回到原来的设置列表。
SIM 写入成功后显示原厂的 3 秒倒计时并重启；当前槽位按钮不可重复选择。
已在 P4 真机验证系统设置入口、当前内置卡回读、刷新和返回；仍需下列入网与电源验收。
其他原厂功能仍需逐项对照，不能用本项测试证明整板适配完成。

## OTA 与网络配置互斥

OTA 下载前通过 `Cellular::TryBeginFirmwareUpdate()` 原子检查并占用网络配置。
Wi-Fi/4G 切换、SIM 查询或切换尚未完成时拒绝 OTA，包括 SIM 成功后的重启倒计时。
占用期间拒绝新的网络配置操作和蜂窝休眠，避免先检查状态、随后被另一个任务切断网络。
本地更新请求和远端更新命令共用此入口；函数退出时由 RAII 释放占用，覆盖参数无效、下载失败、
校验失败、写入失败和无需更新等返回路径。没有蜂窝硬件的板型仍允许原有 OTA。

## 验证

执行 `bash tools/tests/test_firmware_host.sh`，其中
[test_cellular_controller.cpp](../../tools/tests/test_cellular_controller.cpp) 覆盖命令顺序、超时、缺卡、
非法响应、同槽位无操作、失败恢复、请求互斥、队列拒绝、跨休眠恢复取消以及断电失败回滚。
OTA 测试还覆盖两个方向的互斥、释放后恢复操作、Wi-Fi 模式更新，以及 64 轮真实线程竞争。
再执行 `bash tools/check_firmware_style.sh --format-only`、`bash tools/p4.sh build-host`、
`bash tools/s31.sh build-host` 与 `bash tools/s3.sh build-host box3`。

尚需真机确认：两种 SIM 实际入网、未插外置卡时切换内置卡、切换后断电槽位保持、
射频恢复超时后的搜网、休眠恢复与关机行为。Host stub 不替代这些验收。
