# Metalio-Claw4 原厂适配

## SIM 控制

对照原厂 `ca3aa3fa027ff7dad2adf0c2d03c4f24aa838950` 的
`main/display/screen/network_screen/network_screen.cc` 中 `sim_switch_task` 与
`main/boards/common/nt26_board.cc` 中 `SendAtCommand`。

Host 契约为 [Cellular](../../firmware/espressif/main/device/contracts/cellular.hpp)，
板级实现为 [CellularController](../../firmware/espressif/main/platform/boards/metalio-claw4/cellular_controller.cpp)。

- `RequestSimRefresh()` 在既有 BackgroundExecutor 查询 `AT+ECSIMCFG?`，槽位 0 为外置卡、1 为内置卡。
  查询失败或非法响应返回未知状态，不默认显示某张卡。
- `SetSimSlot()` 仅在 4G 模式接受内置或外置槽位。无需联网或初始化完成，缺卡时仍可尝试另一张卡。
- 切换遵循原厂命令顺序和超时：`CFUN=0`（8 秒）、等待 500 毫秒、`ECSIMCFG=SimSlot,X`
  （5 秒）、等待 500 毫秒、`CFUN=1`（15 秒）。首步失败不再写槽位；槽位写入失败仍用
  `CFUN=1`（10 秒超时）尽力恢复射频。与原厂一致，最后一步失败不撤销已成功写入的槽位。
- 成败都回读模组，模组持久化实际槽位。当前接口不维护第二份 NVS 槽位偏好。
- SIM 操作与网络模式切换互斥，并与休眠/恢复通过既有操作锁串行；休眠或关机前排队、尚未执行
  的 SIM 请求取消，不对已停止的模组发送命令。

当前仅完成控制接口；系统 UI 槽位入口、原厂成功后的倒计时重启提示尚未接入，不能视为完整 SIM 功能交付。
其他原厂功能仍需逐项对照，不能用本项测试证明整板适配完成。

## 验证

执行 `bash tools/tests/test_firmware_host.sh`，其中
[test_cellular_controller.cpp](../../tools/tests/test_cellular_controller.cpp) 覆盖命令顺序、超时、缺卡、
非法响应、失败恢复、请求互斥、队列拒绝以及休眠/关机取消。
再执行 `bash tools/check_firmware_style.sh --format-only` 与 `bash tools/p4.sh build-host`。

尚需真机确认：两种 SIM 实际入网、未插外置卡时切换内置卡、切换后断电槽位保持、
射频恢复超时后的搜网、休眠恢复与关机行为。Host stub 不替代这些验收。
