# USB 本地控制协议

USB 本地控制是产品 Host 的开发能力。Metalio-Claw4 使用 ESP32 USB Serial/JTAG，ESP-Mosaico 使用板载
USB 2.0 HS OTG 上的 TinyUSB CDC；两者向 `micropixel` CLI 提供相同的 App 列表、安装、卸载、启动和停止，
不新增 Guest ABI，也不允许原始 Flash、NVS 或任意 Host 函数访问。

## 1. 分层

```text
micropixel CLI
  -> USB Serial/JTAG 或 TinyUSB CDC
  -> DevelopmentLocalControlTransport / LocalControlBackend
  -> LocalControlAgent
  -> RemoteControlAgent 的共享有界 Host 命令队列
  -> HostController
  -> AppStore / BundleFS
```

Platform 只拥有 USB 字节流和开发截图/触摸命令。`LocalControlAgent` 解析 App 管理协议，但不直接
调用 WAMR、LVGL 或 BundleFS。App 生命周期和 Store 变更仍在 HostController 所属任务执行，与 System UI
和 Remote Control 使用同一仲裁顺序。

## 2. MPX1 行协议

请求与响应都是以换行结尾的 ASCII 行：

```text
MPX1 <request-id> <operation> [arguments...]
MPX1 <request-id> OK <result...>
MPX1 <request-id> ERROR <stable-error-code>
```

当前操作：

- `HELLO`：协商协议版本、最大 chunk 和最大 Bundle；
- `APP_LIST <offset>`：按固定上限分页返回 Catalog；
- `APP_START <app-id>`；
- `APP_STOP [app-id]`；
- `APP_UNINSTALL <app-id>`；
- `APP_INSTALL_BEGIN <app-id> <size> <sha256>`；
- `APP_INSTALL_CHUNK <offset> <base64-data>`；
- `APP_INSTALL_COMMIT`；
- `APP_INSTALL_ABORT`。

Bundle chunk 当前最大为 3072 字节，逐块响应下一个 offset，因此 Flash 或 Host 变慢时会自然向 CLI
施加背压。单个 Bundle 上限与 Remote Control 一致，为 8 MiB。设备对安装请求重新校验长度、App ID、
SHA-256、Bundle header、AOT 和资源边界；CLI 提供的字段不能替代设备验证。

当前实现先把完整 Bundle 暂存在 PSRAM，再进入既有 `AppStore::InstallApp`。USB 会话 120 秒无活动后释放
暂存缓冲。后续可把相同协议后端改为直接写 BundleFS staging writer，而不改变 CLI 或 MPX1 命令语义。

## 3. 事务与掉电

`APP_INSTALL_COMMIT` 只表示 USB 传输完成。最终成功响应必须等 Host 完成 BundleFS 写时复制和 Catalog
提交。传输中断、SHA-256 不匹配、Bundle 无效、空间不足或 Host 队列繁忙都不会替换当前已安装版本。

安装和卸载要求没有运行中的 Guest。Host 返回的 `stop_active_app_before_install`、
`stop_active_app_before_uninstall`、`app_not_found`、`no_space` 等稳定错误同时用于远程和 USB 路径。

## 4. JPEG 截图与输入注入

开发截图和输入注入复用同一个行 reader，当前命令为：

```text
MICROPIXEL_CAPTURE LOGICAL
MICROPIXEL_CAPTURE DISPLAY
MICROPIXEL_TOUCH <DOWN|MOVE|UP|CANCEL> <id> <x> <y> <pressure-per-mille>
```

`LOGICAL` 通过 LVGL snapshot 获取逻辑场景；`DISPLAY` 复制 LVGL 当前完整 RGB565 提交缓冲。两种来源都由
SoC JPEG 外设编码为 JPEG，再按“ASCII header + 精确长度二进制 payload + sequence end marker”传输。
这既避免软件 PNG 编码占用 CPU，也能在板上显示异常时区分“LVGL 已经画错”与“提交缓冲正确、面板链路出错”。
Host 工具不会扫描 JPEG 内容寻找结束符，因此压缩数据中即使出现换行或协议文本也不会破坏 framing。

典型命令：

```sh
python3 tools/capture_screen.py "$PORT" logical.jpg --source logical --expect-size 480x480
python3 tools/capture_screen.py "$PORT" display.jpg --source display --expect-size 480x480
```

同一时刻只允许一个进程占用该 CDC 端口。截图暂时持有 LVGL/日志输出锁，完成原始帧复制后立即释放 LVGL
锁；JPEG 编码与 USB 输出不在 LVGL 锁内执行。

## 5. 串口复用与安全

MPX1 与 ESP 日志复用板卡选定的 USB CDC transport。Platform 只在写一条协议响应时持有日志锁，并在
响应前后写换行，CLI 会忽略不匹配 `request-id` 的普通日志。`MICROPIXEL_CAPTURE` 和
`MICROPIXEL_TOUCH` 行命令由同一个 reader 处理，避免多个任务竞争 USB RX。

本地控制以物理 USB 访问作为开发期信任边界，不接受网络 Token。发布策略如需限制本地安装，应在 Host
增加显式开发模式或设备确认，但不能通过降低 Bundle 校验、开放原始分区写入或信任 CLI hash 来实现。

CLI 使用 pyserial，支持 macOS/Linux 设备节点和 Windows `COMx` 端口；自动探测同时识别板卡声明的应用
USB 产品名和 ROM USB 产品名。当前协议已经在 macOS 真机验证，Windows 代码路径受支持但尚未完成项目
真机验证。
