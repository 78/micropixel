# 网络遥测

远程 `device.get_system_info` 的 `network` 包含两种接口的独立状态；默认出口仍由
`transport`（`wifi`、`cellular` 或 `none`）表示。根层的 IP、网关、掩码、DNS、MAC
和 hostname 对应默认出口，不代表所有已连接接口。

- `wifi`：`available`、`enabled`、`connected`；连接后提供 `ssid` 与 `rssi`（dBm）。
- `cellular`：`available`、`enabled`、`connected`；启用后提供 `signalBars`、`csq`、
  `sampled`、`fresh`。CSQ 为 0–31；99 或查询不可用编码为 JSON null。
Wi-Fi 的 `ssid`、`rssi` 仅出现在 `network.wifi` 中，根层不保留同名字段。

蜂窝完整诊断由有界后台任务在 AT 就绪后采集，并每 30 秒更新；信号在已连接时每 5 秒更新。
UI 与远程上报只复制快照，不在其调用路径发送 AT 命令。`sampledAtMs` 是设备启动后的
单调毫秒数，`ageMs` 是完整诊断的年龄。超过 60 秒或正在切卡/切换无线开关时，`fresh`
为 false，不输出身份和小区字段。关闭蜂窝只输出接口状态。

新鲜样本可包含 `carrier`、`imei`、`iccid`、`registration`。标识符保持字符串，
不丢失前导零；ICCID 保留模组返回的十六进制字符。查询失败或格式错误时省略，不复用驱动可能残留的旧 SIM 缓存。
已注册（1 或 5）且字段完整时输出 `cereg: {stat, tac, ci, AcT}`，与 pocket-sage 的
小区字段命名一致。TAC 与 CI 是十六进制字符串。断网、切卡、休眠和模组恢复会使旧遥测失效；
采样期间发生这些状态变化也不能重新发布先前的小区。

当 `COPS` 返回数字运营商格式时，同时提供字符串 `mcc`、`mnc`；不根据 IMSI 或运营商
名称猜测当前注册网络。上报不会修改模组的运营商显示格式。
服务端可将这些信息交给基站定位服务，再使用返回的位置查询天气；本接口不提供经纬度，
也不直接调用天气服务。需要完整 PLMN 的定位服务应检查 MCC/MNC 是否存在，不能仅凭
TAC/CI 假定小区在全球唯一。后台保存远程系统信息时保留这些可选字段，客户端应容忍缺失。
