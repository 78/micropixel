# MicroPixel Remote Control 设计方案

状态：实施中（Implementing）

本文定义 MicroPixel 设备、Control 服务、Web Console 和开发 Agent 之间的远程控制、应用开发与调试方案。它补充系统总体架构，不改变 Guest–Host Service ABI 的基本边界。

## 1. 结论摘要

产品菜单建议使用 **Remote Control**，而不是 Cloud Control。Remote Control 既适用于公共云，也适用于用户自建或局域网 Control 服务；页面副标题可显示当前服务名称，例如 `Connected to MicroPixel Control`。

总体方案如下：

- Remote Control 是 Host 原生系统能力，不是 Guest App，也不新增 Guest Core import；
- 设备通过 HTTP/3 主动建立长期控制连接，命令和结果采用应用层消息，文件使用独立 HTTP/3 stream；
- 设备首次 bootstrap 时由 Control 服务签发 UUID 和设备 JWT，并保存到 `sys_store`；抹除系统 NVS 后会获得新身份；
- UUID 只是公开标识，不能作为 JWT 签名密钥。用户 JWT 由 Control 服务签名，并绑定设备 UUID、身份代次和权限范围；
- 临时连接码由设备界面主动申请，短时、单次使用，并绑定当前设备和网站登录会话；
- App 启停、截图、日志和输入注入通过 Host 内部的有界命令接口完成，网络任务不得直接调用 WAMR、LVGL 或板级驱动；
- App 使用 24 MiB 可写 `app_store`；BundleFS 以离散 64 KiB 数据块写时复制，并在分区内用四个
  16 KiB Catalog Bank 环形提交，生产发布仍需补齐 package 数字签名与断电压测；
- Web Console 同时服务人类和开发 Agent，公开版本化 OpenAPI、Agent 指南、SDK、示例和可复现的 AOT/Package 构建环境。

## 2. 目标与非目标

### 2.1 目标

第一阶段完成以下能力：

1. 在 System Settings 中显示 Remote Control 入口、启用状态、连接状态、临时连接码和访问撤销操作；
2. 网站通过临时连接码关联设备，并签发仅针对该设备的权限凭据；
3. 查看设备状态、当前 App、已安装 App、截图和当前 App 的 Guest 日志；
4. 安装、卸载、启动、停止 App；
5. 模拟触摸和按键，执行包含延迟与截图步骤的操作序列；
6. 为人类和 Agent 提供稳定 API，用于编译、打包、安装、运行、输入、截图和日志闭环；
7. 支持设备身份轮换、Token 撤销、审计、限流和故障恢复。

### 2.2 非目标

首个版本不提供：

- 原始 Flash、任意 NVS、Wi-Fi 密码或设备私钥的远程读写；
- 远程 shell、任意 Host 函数调用或绕过 Service ABI 的 Guest 调试入口；
- 多个 Guest 同时运行；
- 无用户感知的永久远程输入控制；
- 将完整 ESP-IDF 系统日志默认上传到服务端；
- 在 Control API 进程中直接执行不受限制的第三方构建脚本。

## 3. 当前基线与需要补齐的能力

当前项目已经具备可复用的基础：

- `SystemShell` 和共享 `SquareSystemUi` 管理 System Settings；
- `AppController` 管理单一 Guest 的启动、暂停、恢复和停止；
- Bundle v1 reader 可从统一 `app_store` 只读映射并验证 AOT 与资源；
- Guest 的 `log_write` 已有长度限制并进入 ESP 日志；
- Metalio-Claw4 已有基于显示 framebuffer 和 ESP32-P4 JPEG 外设的 Remote Control 截图，以及开发期触摸
  注入实现，并通过 Host 有界队列暴露给控制 Agent；
- `sys_store` 与 Guest `runtime_nvs` 已隔离；
- Guest SDK、示例 App、Wasm/AOT 和 Bundle 构建脚本已经存在。

但以下能力目前仍不足：

- Remote Control 页面、NVS 身份和 TLS peer 验证已有开发实现；生产配置在缺少 CA 或匹配主机名时默认拒绝连接，真机负向安全矩阵仍待验收；
- 配对和 Job 只保存在单个 Fastify 进程内，尚无限流、审计历史和多实例恢复；
- 截图 artifact 已有一小时 TTL、每设备/全局容量上限、周期清理和启动孤儿清理；Guest 日志环形缓冲和
  触摸/语义按键序列已有首版，但流式日志和物理抢占待补；
- 当前开发版 App Store 已使用 BundleFS；离散块回收不依赖连续 extent GC，签名与真机断电恢复
  尚未达到发布门槛；
- 当前 Control API 的内存状态和开发 Token 策略不构成生产账户安全边界。

## 4. 总体架构

```text
Browser / Agent / CLI
        │ HTTPS / HTTP/3
        ▼
┌──────────────── Control 服务 ────────────────┐
│ Caddy: TLS、HTTP/3、静态 Console、反向代理   │
│ API/Auth: 用户、配对、Token、设备元数据      │
│ Device Gateway: 长连接、命令、事件、恢复      │
│ Command Broker: 幂等、超时、结果、审计        │
│ Artifact Store: App 包、截图、构建产物        │
│ Build Worker: 隔离的 Wasm/AOT/Package 构建    │
│ Memory Registry: 在线设备、配对码、Job        │
│ Files: 截图、App Package、构建产物            │
└──────────────────┬──────────────────────────┘
                   │ HTTP/3，设备主动连接
                   ▼
┌──────────────── MicroPixel Host ──────────────┐
│ RemoteControlAgent                            │
│   ├─ Transport / Identity / Pairing           │
│   ├─ 有界 CommandQueue / EventQueue           │
│   └─ Artifact upload/download                 │
│ HostControlService                            │
│   ├─ HostController / AppController           │
│   ├─ AppStore                                 │
│   ├─ AppLogBroker                             │
│   └─ DeviceServices                           │
│       ├─ ScreenCapture                        │
│       └─ InputInjection                       │
└──────────────────┬──────────────────────────┘
                   │ 已有 Service ABI
                   ▼
               单一 Guest App
```

固定依赖方向仍为：

```text
Runtime -> Device contracts <- Platform
```

网络层不能直接依赖 Metalio-Claw4 的 GT911、LVGL 对象或 WAMR instance。所有跨任务操作进入有界队列，并在已有 Host/UI/Runtime 所属任务执行。

## 5. 设备身份

### 5.1 身份记录

首次启用 Remote Control 或首次成功联网时，设备调用 Control 服务的无状态 bootstrap 接口。Control 服务生成身份并返回：

```text
identity_version  = 1
device_id         = 服务端生成的 UUIDv4，公开稳定标识
device_credential = 服务端签名的长期设备 JWT
auth_epoch        = 1
issued_at         = 服务端签发时间
```

`device_credential` 至少包含 `typ=device`、`sub=device_id`、`scope=device:connect` 和签发时间。Fastify 使用持久化在环境变量或受限文件中的服务端密钥验证签名，不保存设备记录，也不查询数据库。所有 API 启动入口必须加载同一份稳定密钥；缺少密钥时服务拒绝启动，不得回退到进程内随机密钥。

UUID 必须由服务端生成，bootstrap 不接受调用方指定目标 UUID。否则在没有数据库记录唯一性的情况下，攻击者可以请求服务端为已知 UUID 再签发一份合法设备凭据。

设备凭据应优先受 Flash Encryption 和 Secure Boot 保护；未启用这些能力的开发固件必须在 UI 和服务端标记为 `development security`。后续版本可升级为设备端 P-256 密钥和 challenge-response，但不阻塞无数据库 MVP。

不要把身份字段追加到现有 UI 设置的定长记录中。身份需要独立版本、独立校验和独立迁移，避免亮度或音量设置迁移破坏设备身份。

### 5.2 无状态 bootstrap 与认证

1. 设备校验 Control 服务 TLS 证书和主机名；发布版本不允许跳过证书验证；
2. NVS 中没有身份的新设备调用 `POST /device/v1/bootstrap`；
3. Fastify 生成新 UUID，并使用服务端密钥签发长期 `device_credential` JWT；
4. 设备把 UUID、credential 和 identity version 原子写入 `sys_store/control`；
5. 后续设备连接使用 `Authorization: Device <device_credential>`；
6. Fastify 无状态验证 JWT，并把已认证连接加入内存在线设备表；
7. 服务重启后内存表丢失，设备按退避策略自动重连；Control stream 返回 `401` 时，设备将其视为长期设备凭据失效，清除 NVS 中的旧身份并重新 bootstrap；其他非 `200` 状态保留身份并重试。

bootstrap 接口可以公开，但必须限流；每次调用只能创建一个全新身份，不能签发指定 UUID。该模型验证的是“持有服务端签发的设备凭据”，暂不证明真实硬件来源。

### 5.3 NVS 抹除和身份轮换

抹除 `sys_store` 后，设备 UUID 和 credential 消失。下一次连接会重新 bootstrap，获得新 UUID，因此服务端将其视为新设备。绑定旧 UUID 的 Token 不能控制新 UUID。

无数据库版本不保存旧设备撤销表：旧 credential 在过期前理论上仍代表旧 UUID，但它不能代表新 UUID。设备 credential 应设置较长但有限的期限，并在后续版本增加刷新和撤销机制。

提供两个不同操作：

- **Revoke all access**：递增设备 NVS 中的 `auth_epoch`，使当前在线会话拒绝旧 epoch 的控制 JWT；
- **Reset Remote Control identity**：清除 UUID 和 credential 并重新 bootstrap；完整抹除系统 NVS 也产生此效果。

第一版强撤销依赖短期控制 JWT、当前在线设备的 epoch 检查或 identity reset。若将来要求服务重启后仍能逐 Token 即时撤销，则需要持久撤销表或数据库。

## 6. 临时连接码与配对

连接码不是 API Token，也不能直接执行控制命令。

推荐流程：

1. 用户在设备 Remote Control 页面点击 `Generate Connection Code`；
2. 已认证的设备请求一次性 pairing challenge；
3. 服务端生成 `pairing_id` 和人类可读的连接码，例如 `7K4M-P9QF`；
4. 服务端只在内存中保存连接码的带密钥摘要，绑定当前在线 `device_id`，默认 5 分钟过期、单次使用；
5. 设备显示连接码、倒计时、服务域名和 Cancel/Regenerate；
6. 用户在网站输入连接码；
7. 服务端原子消费内存中的连接码并直接签发设备范围、有效期 1 小时的 Console Token；
8. 设备立即收到 `paired` 事件。服务重启会使尚未消费的连接码失效。

安全限制：

- 推荐至少约 40 bit 的随机连接码；6 位纯数字只能用于更短过期时间和更严限流；
- 每台设备同时只有一个有效码，重新生成会使旧码失效；
- 每个 pairing、IP、账户和设备都要限制失败次数，默认 5 次；
- 配对码不能写日志，不能出现在 URL query，不能被分析系统采集；
- 生成连接码必须是设备上的显式物理操作；
- 第一版不要求网站账户；Console Token 仅用于浏览器会话，API Token 只展示一次；增加账户系统后再持久化设备绑定关系。

## 7. 用户凭据、JWT 与权限

### 7.1 三类凭据

不要用一个 Token 同时承担所有角色：

1. **设备凭据**：bootstrap 签发的长期设备 JWT，用于设备到 Gateway 的认证；
2. **Console Token**：配对成功后给浏览器使用，有效期 1 小时；
3. **API Token**：由 Console Token 签发给用户或 Agent，按设备和 scope 限权，有效期可选 7 天、30 天或永久，且不能继续签发 Token。

无数据库版本不能可靠地逐 Token 即时撤销。Console Token 固定短时有效；所有 API Token 都包含
`device_epoch`，永久 Token 只能通过设备 identity reset/auth epoch 轮换或服务端签名密钥轮换整体失效。
如果需要逐 Token 即时撤销，则增加只保存 Token 哈希的持久撤销表。

### 7.2 JWT 语义

UUID 是 JWT 的目标标识，不是签名密钥。JWT 由 Control 服务的轮换签名密钥签发，建议 claims：

```json
{
  "iss": "https://control.example.com",
  "aud": "micropixel-control",
  "sub": "user-or-agent-id",
  "device_id": "d89c...",
  "device_epoch": 3,
  "scope": ["device:read", "app:manage", "logs:read"],
  "jti": "...",
  "iat": 1787600000,
  "exp": 1787603600
}
```

Gateway 校验签名、时间、audience、目标 `device_id`、当前在线连接报告的 `device_epoch` 和 scope。第一版不维护持久 `jti` 撤销表。

### 7.3 权限范围

建议最小 scope 集合：

| Scope | 权限 |
|---|---|
| `device:read` | 连接状态和经过脱敏的系统信息 |
| `device:manage` | 重启设备等需要显式确认的系统级操作 |
| `app:read` | App 列表、当前状态和版本 |
| `app:install` | 上传、验证、安装和卸载 App Store 条目 |
| `app:manage` | 启动、停止、暂停和恢复 App |
| `screen:read` | 获取截图 |
| `logs:read` | 获取当前 App 的 Guest 日志 |
| `input:inject` | 模拟触摸和按键 |
| `system:diagnostics` | 额外 Host 诊断信息，默认不授予 |

`app:install` 和 `input:inject` 属于高风险权限，应单独选择并在设备端显示活跃控制指示。首个版本不提供控制 Wi-Fi、修改系统 NVS 或读写原始存储的 scope。

## 8. System Settings 中的 Remote Control

### 8.1 菜单接入

在 `SystemMenuItem`、`SystemUiActionType`、`SystemUi` 和共享详情页中增加 Remote Control。它与 Wi-Fi、System Information、Manage Apps 同属 Host 原生 UI。

System Settings 行显示摘要：

```text
Remote Control                  Connected
```

或：

```text
Remote Control            Off / Connecting / Error
```

### 8.2 详情页

详情页至少显示：

- Enabled 开关；生产固件默认关闭，开发固件可通过 Kconfig 改默认值；
- 状态：Off、Waiting for network、Connecting、Connected、Backoff、Auth error；
- Control 服务域名和连接持续时间；
- 最近成功连接时间与最近错误的简短、可理解描述；
- 已关联账户和有效访问数量；
- `Generate Connection Code`、请求中的禁用状态和 spinner、连接码与倒计时；
- 关闭 Remote Control 的入口置于页面底部，并在执行前二次确认；
- `Revoke all access`；
- `Reset Remote Control identity`，二次确认；
- 开发安全状态，例如 TLS 验证、Secure Boot、Flash Encryption；
- 当前有远程控制者时的明显指示。

状态栏在远程查看屏幕、日志或注入输入时显示 Remote 图标。用户的物理输入优先于远程输入；电源键、系统返回手势和退出 Remote Control 始终可用。

### 8.3 本地持久设置

Remote Control 设置存于 `sys_store/control`：

- enabled；
- service profile 或受信任服务标识；
- device identity；
- auth epoch；
- 可选的首次高风险控制需设备确认策略。

不在 NVS 保存浏览器 JWT、配对码或服务端用户密钥。

## 9. 设备端模块设计

建议新增 Host 模块：

```text
firmware/espressif/main/host/controller/
  control_types.hpp              # Local/Remote 共用的传输无关命令和快照
  control_dispatcher.*           # 有界队列、结果路由和生命周期通知
  remote/
    remote_control_agent.*       # 远程生命周期、HTTP/3 状态机和 artifact 传输
    remote_control_protocol.*    # 版本化 wire schema
    remote_identity_store.*      # NVS 身份与设备 credential
    remote_guest_log_buffer.*    # Guest 日志 ring 和 cursor
    remote_reconnect_policy.hpp  # 可测试的重连/退避策略
```

后续如果 HTTP/3 transport 或 artifact 流程继续增长，应在 `remote/` 内再拆独立实现文件，但仍只通过
`ControlDispatcher` 与 Host Controller 交换命令和结果。

### 9.1 任务与队列

- 网络任务只解析有上限的消息并投递命令；
- `HostCommandQueue` 在 HostController 所属上下文执行 App 生命周期操作；
- UI/截图操作进入 LVGL 所属上下文；
- 输入注入进入统一 Input Router，再经过系统手势和焦点路由；
- App 下载和截图上传使用独立 stream，不阻塞控制 stream；
- 所有队列、消息、并发 Job、日志和 artifact 都有编译期或 Kconfig 上限；
- 不使用 detached task，不让网络路径隐式扩容。

内存分层采用“动态大 payload 进 PSRAM、固定控制元数据留内部 RAM”：HTTP/3 动态 buffer、截图 JPEG、
Package staging 和 Host command/result storage 优先分配 PSRAM；队列句柄、锁、任务栈和小型状态机保持在内部
RAM。PSRAM 不能代替任务栈；Remote Control 不重新执行 LVGL snapshot，而是短暂冻结 panel framebuffer，
再由 JPEG 外设直接读取 BGR24 frame。这样既避开复杂 System UI 在 `lv_snapshot` 软件混色路径上的深栈，
也不再为一张截图同时保留 raw snapshot 和 libpng 工作内存。

建议初始上限：每设备同时 1 个输入序列、1 个截图、1 个安装 Job、8 个待执行命令。最终数值要根据 PSRAM/Flash 压测确定。

### 9.2 重连状态机

```text
Disabled
  └─ Enabled -> WaitingForNetwork -> Connecting -> Authenticating
       -> Online -> Backoff -> Connecting
```

使用带随机抖动的指数退避；bootstrap 失败、Control stream 连接失败、任意非 `200` 响应、异常关闭和读取错误都必须进入退避，不得固定频率重试。当前退避上限依次为 10、20、40、80、160、300 秒，每次实际等待使用硬件随机源落在该档位的 50%–100%（即首次 5–10 秒，封顶后每次 150–300 秒）；只有收到有效控制流数据才重置退避。网络切换、服务器重启和 QUIC idle timeout 不得造成忙循环。连接恢复后以新 `sessionId` 发送 `device.hello` 和完整设备快照，未完成 Job 由服务端按 replay policy 重投。

## 10. HTTP/3 控制协议

### 10.1 通道划分

- 长期 `control` stream：服务端到设备的命令；
- `events` stream/request：设备到服务端的状态、命令进度和结果；
- 独立下载 stream：App Package；
- 独立上传 stream：截图、诊断 artifact；
- 日志由 Console 按 cursor 创建批量拉取命令，服务端只通过已有设备控制流中转；Console 首次打开时同步
  一次，之后仅在日志对话框打开时持续拉取，避免设备常态主动上传和每条日志一个请求。

QUIC ACK 只表示传输层数据到达，不能代表“App 已启动”或“截图已生成”。每条控制命令都必须有应用层状态和最终结果。无需为每个传输 ACK 再发业务 ACK；设备只需发送命令状态、周期状态和必要的应用层 keepalive。

### 10.2 命令 envelope

首版使用有长度上限的 NDJSON，后续可升级为 length-prefixed CBOR。当前 v1 wire envelope 为：

```json
{
  "protocolVersion": 1,
  "type": "command",
  "sessionId": "control-session-uuid",
  "commandId": "018f...",
  "name": "input.sequence",
  "timeoutMs": 60000,
  "params": {}
}
```

设备返回：

```json
{
  "protocolVersion": 1,
  "type": "command.completed",
  "sessionId": "control-session-uuid",
  "deviceBootId": "device-boot-uuid",
  "eventId": "event-uuid",
  "eventSequence": 41,
  "commandId": "018f...",
  "outcome": "succeeded",
  "result": {
    "message": "sequence_completed",
    "screenshots": []
  }
}
```

Fastify 根据 `command.accepted/progress/completed` 把 Job 推进为
`queued | dispatched | accepted | running | succeeded | failed | cancelled | expired | indeterminate`。
所有事件都绑定当前 control session 与 device boot；`eventSequence` 在每个 control session 从 1 开始单调递增，
服务端拒绝倒退但允许缺口，因为设备可能在本地丢弃可再生的 snapshot/progress 事件。`eventId` 用于重传去重，
`commandId` 用于命令生命周期关联。错误码保持机器可读，例如 `device_offline`、
`permission_denied`、`device_busy`、`package_invalid`、`deadline_exceeded`。

### 10.3 幂等与恢复

当前无数据库单实例提供的是**进程内、至少一次投递**，不能宣称跨进程持久恢复：

- Fastify 在同一进程内保留所有未完成 Job；设备 control stream 重连并完成 `device.hello` 后按 replay policy 重新投递；
- `Idempotency-Key` 只在当前内存窗口内去重，服务重启会丢失 Job、最终结果和去重窗口；
- 设备在本次 boot 内保存最近 16 个 `commandId` 和最多 4 个完成结果；重复命令重放 accepted/final 状态而不再次执行；设备重启后窗口丢失；
- `app.start`、`app.stop`、安装同一版本和卸载不存在 App 应保持状态幂等；输入序列不是天然幂等，设备在
  “已执行但结果尚未送达”后重启仍可能再次执行，生产版本需要持久完成记录或服务端恢复协议；
- `timeoutMs` 是服务端投递当时剩余的设备执行预算，不是 wall-clock 截止时间；设备转换为 FreeRTOS 单调 deadline，并在开始执行及序列步骤间复核；CLI 的 `--wait-timeout` 仅控制客户端等多久，不取消设备端 Job；
- 后续增加数据库时持久化 `command_id`、idempotency key、目标身份代次和最终结果，但不改变 v1 envelope。

### 10.4 心跳

QUIC 自身负责链路和重传，但双方仍需要识别应用是否存活：

- 服务端在 `session.ready` 后立即发送 `ping`，之后按协商间隔发送带当前 `sessionId` 的 `heartbeat`；
- 设备不需要逐帧业务 pong；状态变化通过 `device.snapshot` 事件上报，没有变化时依赖 HTTP/3/QUIC keepalive；
- `ping/heartbeat` 的服务端 UTC 只用于观测，不能替代设备单调时钟或校准系统时间。

间隔应由服务端下发并允许固件设最小值，避免被配置成高频耗电轮询。

### 10.5 当前 HTTP/3 传输约束

当前 `esp-http3` 组件对大于 1 KiB 的 POST body 自动切换到 streaming upload，以 1 KiB DATA chunk 发送，
chunk 间至少让出一个 FreeRTOS tick，避免把 180 KiB 级截图塞入单个固定 frame buffer。组件必须同时满足：

- 所有已认证 1-RTT 包（包括纯 ACK）都推进最大接收 packet number；纯 ACK 不触发新的 ACK，避免 ACK loop；
- Key Phase 变化时先用当前/上一代密钥尝试，再用下一代接收密钥验证；只有认证成功后才提交 peer key update；
- QUIC Key Update 只轮换 application traffic secret、AEAD key 和 IV，header-protection key 按 RFC 9001
  Section 6.1 保持不变；否则服务端切换 Key Phase 后会立即出现 AEAD failure；
- 大 body、control stream 和 event request 使用同一连接时，任何传输失败都必须变成应用层
  `artifact_upload_failed`，不能把截图 Job 误报为完成。
- 设备端连接为辅助 HTTP/3 请求保留最多 10 个并发 stream；每个 stream 使用固定上限、优先位于 PSRAM 的
  接收 buffer，并由连接级 flow-control 总窗口约束。状态上报不得同步阻塞 control stream 或本地配对命令。

ESP-IDF 6.1 当前解析到的 `esp_hosted` 2.12.12 使用 `ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1` Kconfig；
产品 defaults 必须显式配置 Metalio-Claw4 的 CLK/CMD/D0/D1/D2/D3 引脚，不能只依赖旧版 symbol 或本机
生成的 `sdkconfig`。

## 11. 远程功能设计

### 11.1 系统信息

复用 Host 的系统信息模型并生成不可变快照。建议字段：

- 设备 UUID、型号、硬件 revision；
- Firmware、ESP-IDF、Remote Control protocol、Guest ABI 和 SDK 版本；
- 芯片、CPU、内部 SRAM、PSRAM、Flash、可写 App Store 使用量；
- 屏幕型号、分辨率、触摸能力；
- 网络状态、RSSI、IP 协议和 HTTP/3 会话状态；
- uptime、reset reason、电池/电源状态；
- 当前 App ID、版本、session ID 和生命周期状态。

MAC、IP 和 reset detail 等敏感字段默认脱敏；只有适当 scope 才返回完整值。

设备协议将诊断拆成两个只读指令：`device.get_system_info` 返回 Firmware、硬件、内存、存储、运行状态和网络快照；
`device.get_task_diagnostics` 按需采集 FreeRTOS 任务、区间 CPU、优先级、Core affinity 和最低剩余栈。
每个任务用 `coreId` 表示亲和性：`0` 或 `1` 表示固定到对应 Core，`null` 表示未绑定、可在所有 Core
运行；字段缺失表示当前 Firmware 未采集该信息，调用方不得把它解释成未绑定。
任务采样不附带在常规系统信息中，避免 Console 的常规轮询反复暂停调度器并构造较大的 JSON。CLI 的
`micropixel device diagnostics` 依次执行两个指令并合并展示，保持单一的开发者入口。

### 11.2 App 列表与生命周期

App 列表中的所有条目都来自统一 App Store；设备没有预置 App，所有 Bundle 具有相同的启动、升级和卸载语义。
Control 协议的 `source` 固定为 `app_store`；运行状态使用 `running`、`suspended`、`stopped`、
`installing`、`broken` 等值。擦除 `sys_store` 不影响 BundleFS Catalog；清空 App 必须显式格式化
`app_store`。

启动与停止继续由 `AppController` 执行，保持同时最多一个 Guest：

- 启动当前已运行 App 是幂等成功；
- 启动另一个 App 默认先执行有界停止，再启动新 App；也可由 API 明确要求 `replace=false` 并返回 conflict；
- 停止先走正常 shutdown，超时后才允许 force stop；
- 安装、升级和卸载运行中的目标 App 前必须停止，并等待 Bundle mapping 释放；
- UI 与远程命令竞争时，通过同一个 Host 仲裁器决定顺序。

### 11.3 截图

把现有截图实现拆分为传输无关的 `ScreenCapture` Presentation 接口：

- Metalio-Claw4 `ScreenCapture` 实现负责冻结当前显示 framebuffer，并用 ESP32-P4 JPEG 外设编码；
- USB 开发截图和 Remote Control artifact 均使用 SoC JPEG 外设编码；USB transport 使用带长度的二进制
  JPEG framing，并可选择 LVGL 逻辑场景或显示提交缓冲；
- 同时只允许一个截图 Job；
- framebuffer 冻结、JPEG 编码和网络上传分阶段执行，避免持有显示所有权等待网络；
- 大缓冲优先使用 PSRAM，并设置最大像素、最大输出和超时；
- 服务端返回短时签名 URL，截图内容不塞进控制消息。

artifact 元数据至少包含 `artifact_id`、截图步骤 ID、尺寸、MIME、SHA-256、创建时间和过期时间。

### 11.4 输入注入与操作序列

Remote Control 只调用硬件无关的 Input Service。虚拟触摸先经过 Platform 坐标校验，再回到 System Gesture Router；语义按键由 System Gesture Router 投递给当前 Guest，不能让 Remote Control 直接调用 GT911 或 Guest ABI。

当前已落地的底层操作序列示例：

```json
{
  "name": "input.sequence",
  "params": {
    "operations": [
      {"type": "touch", "phase": "down", "id": 0, "x": 360, "y": 620},
      {"type": "touch", "phase": "up", "id": 0, "x": 360, "y": 620, "delayMs": 80},
      {"type": "key", "code": "confirm", "phase": "down", "delayMs": 120},
      {"type": "key", "code": "confirm", "phase": "up", "delayMs": 80},
      {"type": "screenshot", "id": "after-input", "delayMs": 500}
    ]
  }
}
```

当前支持 `down | move | up | cancel` 原始触摸、`down | up | repeat | cancel` 语义按键和带 ID 截图；
固定键码为 `up/down/left/right/confirm/back/menu/south/east/west/north`。每个序列最多 16 步、4 张截图、单步延迟
5 秒、总延迟 10 秒。成功、失败或 deadline 到期后，Host 都对仍处于按下状态的触点和按键补发
`cancel`。tap/swipe 可由这些底层步骤组合。Host 使用跨循环的有界序列状态机，每次只推进一个步骤，让
System UI 或 Guest 在步骤之间消费输入；状态机会跨 Remote Control、System Settings、App Hall 和
Foreground App 继续运行，因此后续带 ID 的截图观察到的是输入处理后的画面，而不是入队前画面。设备端限制：

- 最大步骤数、总时长、单步 delay 和并发触点；
- 每台设备同时只有一个输入序列；
- 失败、超时或序列自然结束时补发 synthetic cancel；
- 物理输入可抢占远程序列；
- 受保护的 System Settings 操作可要求设备确认，不能用远程输入关闭安全提示。

序列是异步 Job。HTTP API 可提供 `wait=true` 的便利模式，但内部仍是 command + result。最终结果按截图步骤 ID 返回 URL：

```json
{
  "job_id": "...",
  "status": "succeeded",
  "steps": [
    {"id": "after-start", "status": "succeeded", "screenshot_url": "https://..."},
    {"id": "after-back", "status": "succeeded", "screenshot_url": "https://..."}
  ]
}
```

### 11.5 Guest 日志

在现有 `log_write` Host 实现旁增加 `AppLogBroker`，不新增 Guest Core import。每条记录包含：

```text
sequence, monotonic_timestamp, wall_timestamp?, level,
app_id, app_version, session_id, message, truncated
```

要求：

- 固定容量 ring buffer，按记录和总字节双重限制；
- App 启动产生新 session，停止时封存游标；
- 缓冲溢出时丢弃最旧记录并报告 dropped count；
- Console 首次打开或日志对话框可见期间按不透明 cursor 创建后台 `logs.read` Job，设备返回增量批次；
- 日志对话框内返回 `hasMore=true` 时 Console 立即续拉，否则等待 3 秒；服务端不拥有轮询定时器；
- Console 复用已有 NDJSON 实时流接收合并后的快照；REST API 用 cursor 读取服务端缓存；
- 默认只提供 Guest 日志，不上传可能含凭据的完整 Host 日志；
- 日志在服务端有容量、保留期和主动删除策略。

### 11.6 固件发现与 OTA

Control 用 `control/firmware-release.jsonc` 配置唯一的最新发布版本、通道、固件镜像相对路径、发布时间和
更新说明；部署环境可通过 `CONTROL_FIRMWARE_RELEASE_FILE` 切换配置文件。服务端读取镜像后计算大小和
SHA-256，以内容 hash 作为不可变下载地址，不信任 JSONC 中手写的大小或摘要。

设备在 Wi-Fi 和可信 TLS 条件满足后立即检查更新，此后每 15 分钟检查一次；网络恢复后立即重查。这个检查
不依赖用户是否开启 Remote Control 控制功能。发现比当前嵌入版本新的固件时：

- App Hall 右上角显示带红点的 `Update` 快捷入口，点击直接进入 System Information 的升级界面；
- System Settings 的 System Information 行显示更新状态，详情页提供安装按钮；
- Web Console 的系统信息卡显示目标版本，并通过带结果和超时语义的 `firmware.update` Job 发起升级。

设备把镜像下载到有上限的 PSRAM staging，复核长度和 SHA-256 后分块写入非活动 OTA 分区；写完后再次读取
ESP image descriptor，要求镜像内版本与 JSONC 版本完全一致，才切换启动分区并重启。新固件完成 Platform
初始化后调用 ESP-IDF rollback 确认接口；启动失败时由 OTA rollback 保留可恢复路径。JSONC 版本和构建时的
`PROJECT_VER` 必须同步提升。

## 12. 可安装 App Package 与统一 App Store

### 12.1 BundleFS 单分区模型

Guest 无法直接访问 Host Flash，也不能绕过 Bundle reader 获得 App Store 地址。当前 ESP32-P4 使用一个
24 MiB 可写 `app_store`，其底层格式是 [BundleFS](bundlefs.zh-CN.md)：

```text
app_store metadata  64 KiB；四个 16 KiB Catalog Bank
app_store data      383 个 64 KiB 数据块
PSRAM               当前下载 staging，单个 package 上限 8 MiB
```

BundleFS 不使用 NVS Catalog，也没有预置 App。全擦除态分区首次挂载时只创建空 Catalog；Blocks、Snake
和 Demo 与其他 Bundle 一样通过安装事务进入 Store。擦除 `sys_store` 只轮换设备身份和系统设置，不改变
已安装 App；清空 App 必须显式格式化 `app_store`。

Catalog 最多保存 50 个 App。每个 Bundle 的有序物理块号只保存在 Catalog，数据块本身没有链表头；同一
Bundle 的块可以离散分布，并通过 `spi_flash_mmap_pages()` 映射为连续虚拟地址。卸载后的块可直接复用，
不需要为连续 extent 空洞执行搬迁 GC。

Bundle 运行时保持只读映射。当前 Host 只在 Guest 已停止且 Hall cover mapping 已释放时安装、更新或卸载，
避免重新分配仍被 WAMR、LVGL 或资源视图引用的旧块。

### 12.2 安装事务

1. 网站上传 package，服务端计算 SHA-256 并保存 artifact；
2. 服务端生成设备绑定的 package ID，设备只能用自己的 Device credential 下载；
3. 当前设备完整下载到 PSRAM staging；目标版本改为边下载边调用 BundleFS writer；
4. 校验 Bundle header、大小、ABI、AppId、section hash 和 SHA-256；版本、capabilities 与数字签名待补；
5. BundleFS 分配未被 active Catalog 引用的数据块，擦除并写入新 Bundle；
6. 逐块读回，重新验证 Bundle/AOT/resource 边界和 SHA-256；
7. 擦除环形序列中的下一个 Catalog Bank，写入 generation 加一的新 Catalog 和 checksum；
8. 读回验证后最后写 commit marker，新 Catalog 才成为可见状态。

四个 Bank 每个保存一代完整 Catalog，没有 Bank 内 slot、同代镜像或 `retired_marker`。挂载时选择 CRC 和
commit marker 有效且 generation 最大的 Bank。提交前掉电继续选择旧 Catalog；目标 Bank 擦写期间掉电
也不会破坏上一代；提交后只引用完整的新数据块。卸载通过新 Catalog 删除条目，旧块随后成为空闲块。
尚需在真机上覆盖数据块 erase/write/verify 和 Catalog Bank erase/write/commit 的逐断电点矩阵。

### 12.3 Package 格式和签名

现有 Bundle 中的 FNV hash 适合发现损坏，不提供来源真实性。远程安装必须使用带 SHA-256 和数字签名的外层 `.mpxapp` package，或定义兼容的 Bundle v2：

```text
manifest:
  package_version
  app_id / app_version
  guest_abi_version / sdk_version
  entrypoint / capabilities
  payload_sha256 / payload_size
  signer / signature
payload:
  existing Bundle v1 or future Bundle
```

开发模式可信任用户账户的临时签名；生产发布使用开发者签名或服务端审核签名。设备必须拥有受信任公钥或可验证的签名链。

Catalog 当前固定容量为 50 个 App；本机 UI 需要分页，但仍不能在实时路径无界扩容。

## 13. Control 服务设计

### 13.1 服务组件

`control/` 最终建议包含：

```text
control/
  etc/             Caddy、环境模板、开发证书说明
  apis/            TypeScript API/Auth/Device Gateway
  console/         Web Console 和文档 UI
  workers/         隔离 Build Worker、artifact maintenance
  schemas/         OpenAPI、设备协议和 JSON Schema
  data/             开发期 artifact，默认不提交
```

Caddy 负责 TLS、HTTP/3、静态文件和反向代理。TypeScript 服务负责认证、业务 API、设备 Gateway 和命令状态。Caddy 不替代 TypeScript 业务逻辑。

第一版明确不使用数据库：Fastify 进程只持有内存 Registry，截图、App Package 和构建产物保存到本地 artifact 目录。唯一必须跨重启保存的服务端秘密是 JWT 签名密钥，它来自环境变量或权限受限的密钥文件，不能在每次启动时随机变化。

该模式只支持单个 Fastify 实例。出现账户系统、历史审计、逐 Token 撤销、多实例路由或长期设备列表需求时，再引入持久数据库和 broker；第一版协议不依赖数据库内部 ID，后续迁移不改变设备 wire schema。

### 13.2 内存状态与文件

- `onlineDevices: Map<device_id, DeviceConnection>`：连接、auth epoch、状态、last seen；
- `pairingChallenges: Map<code_digest, PairingChallenge>`：设备、过期、失败次数；
- `jobs: Map<job_id, CommandJob>`：服务端内部排序、idempotency、deadline、状态和结果；内部 delivery attempt 不进入设备 wire envelope；
- `recentEvents`：每设备固定容量的调试环形缓冲区；
- `artifacts/`：截图、Package 和构建产物，文件名使用随机 artifact ID；
- `artifact manifest`：与文件同目录的小型 JSON 元数据，可由文件扫描重建。

内存 Map 仍必须设置数量、TTL 和周期清理。服务重启后在线状态、配对码、Job 和最近事件会丢失；设备自动重连并重新上报快照。秘密、完整 Token 和连接码不能进入日志或 artifact manifest。

## 14. 对外 API 草案

所有接口以 `/api/v1` 版本化，OpenAPI 是事实来源。下表只定义资源方向：

| Method | Path | 用途 |
|---|---|---|
| `POST` | `/device/v1/bootstrap` | 为新设备签发 UUID 和设备凭据 |
| `GET` | `/device/v1/devices/{id}/control` | 设备认证后的长期控制流 |
| `POST` | `/device/v1/devices/{id}/pairings` | 在线设备申请临时连接码 |
| `POST` | `/pairings/claim` | 网站提交临时连接码 |
| `GET` | `/devices` | 当前 Token 可见且在线的设备 |
| `GET` | `/devices/{id}` | 设备信息和在线状态 |
| `GET` | `/devices/{id}/stream` | Console Token 认证的 NDJSON 实时快照、Job 和事件流 |
| `POST` | `/tokens` | 从有效控制凭据签发更窄 scope 的 Agent JWT |
| `GET` | `/devices/{id}/apps` | App 列表和当前状态 |
| `POST` | `/devices/{id}/apps/install` | 从 package artifact 安装 |
| `DELETE` | `/devices/{id}/apps/{appId}` | 卸载 App |
| `POST` | `/devices/{id}/apps/{appId}/actions/start` | 启动 App |
| `POST` | `/devices/{id}/apps/{appId}/actions/stop` | 停止 App |
| `POST` | `/devices/{id}/screenshots` | 创建截图 Job |
| `POST` | `/devices/{id}/input-sequences` | 执行输入/截图序列 |
| `GET` | `/devices/{id}/jobs/{jobId}` | 获取命令进度和结果 |
| `GET` | `/devices/{id}/logs` | 按 session/cursor 获取日志 |
| `POST` | `/devices/{id}/packages` | 上传并校验设备绑定的 package artifact |
| `GET` | `/device/v1/devices/{id}/packages/{packageId}` | 设备认证下载 package |
| `POST` | `/builds` | 创建隔离构建 Job |
| `GET` | `/builds/{id}` | 构建状态和产物 |
| `GET` | `/sdk/releases/latest` | 当前 SDK 元数据与版本化下载地址 |

所有创建 Job 的接口支持 `Idempotency-Key`。默认返回 `202 Accepted + job_id`；客户端等待超时不取消设备端 Job，之后可按 `job_id` 继续查询，CLI 使用 `micropixel job wait <job-id>` 恢复等待。

设备 Gateway 使用独立的 `/device/v1` 路由和 `typ=device` 凭据，不能接受 `typ=control` 的用户 JWT 伪装设备。

## 15. Web Console 信息架构

### 15.1 页面与对话框

公开网站首页固定为 `/`，承载产品定位、SDK 能力和 Quickstart 入口；Developer Docs 位于 `/docs/*`；
需要配对的设备控制台固定为 `/console/`，不再占用网站根路径。

1. **Connect Device**：未登录或 Console 会话失效时只显示连接码输入、首页和文档入口；验证失败显示明确错误；
2. **Device Overview**：登录后的唯一工作台页面，第一行放当前 App 和系统信息，第二行放最新屏幕截图/交互入口
   和带滚动的已安装 App 列表；任务列表、固件升级和重启设备依次位于系统信息卡片右上角，不使用功能 Tab；
3. **Apps 对话框**：App Store 列表、安装、升级、卸载、启动、停止；
4. **Screen & Input 对话框**：定时截图，鼠标点击/拖动映射为触摸，语义按键和手动刷新；
5. **Logs 对话框**：级别与文本过滤，日志列表独立滚动并使用不可展开的紧凑单行；
6. **Tasks 对话框**：紧凑展示 FreeRTOS 任务、区间 CPU 和精确字节的最低剩余栈；
7. **API Token 对话框**：右上角入口，默认签发全部设备权限，Token 只展示一次；
8. **Firmware Update 对话框**：由系统信息卡片右上角进入，显示当前版本、目标版本、大小、SHA-256 摘要和更新说明，确认后创建可跟踪的 OTA Job；
9. **Developer / Agent Docs**：SDK、构建、API 和完整调试闭环；
10. **全局会话操作**：Console 顶部始终提供返回网站首页和退出登录；退出会清除浏览器保存的 Console Token，
    停止当前实时流并回到 `/console/` 的连接码页面。

### 15.2 实时状态

浏览器通过 Console Token 认证的版本化 NDJSON 长连接接收设备 presence、App 状态、Job 进度和事件；
断线后按指数退避重连。设备控制流建立时服务端立即发送带 `utcTimeMs` 的 `ping`，后续 heartbeat 继续携带
UTC 时间；这些字段只作为 Control 协议的补充观测，不参与设备校时。大厅在 localization 配置落地前固定
按 UTC+8 显示 `HH:MM`；TLS 不依赖该时间，产品关闭 `CONFIG_MBEDTLS_HAVE_TIME_DATE` 并忽略证书
`notBefore`/`notAfter`。
连接建立时 Console 自动请求系统信息、App 列表和一批日志，任务诊断仅在 Tasks 对话框打开时请求；服务端 heartbeat 主动
结算超时 Job。无对话框时，浏览器活跃状态每 10 秒同步系统与 App 信息，不活跃时降为每 60 秒；屏幕、任务、
应用或日志对话框打开时每 3 秒同步对应数据，日志 `hasMore` 时不等待并立即续拉。浏览器连续 5 分钟无活动会
自动关闭这些高频对话框。
设备不主动推送日志。浏览器到服务端不复用设备控制 stream，也不能直接连接设备。

UI 上所有危险按钮显示目标设备、目标 App 和权限；卸载、identity reset、force stop 等需要二次确认。

## 16. 面向 Agent 的开发文档

Agent 页面既要能读，也要能被工具稳定解析：

- `/docs/agent`：简洁的任务导向说明；
- `/docs/agent/index.json`：版本、下载地址、hash、示例和入口；
- `/openapi/v1.json`：所有网站 API；
- `/schemas/device-control-v1.json`：命令、事件和错误 Schema；
- SDK archive、SHA-256、release notes；
- 可复制的 CLI 示例和最小 App；
- Blocks、Snake、Demo 的源码链接与说明；
- Guest 限制：单线程、无 syscall、无裸硬件、固定 Service ABI；
- Token scope、安全限制、幂等和日志/截图保留策略。

文档按目标组织，而不只是列 API：

1. 从 Console 创建具有最小必要 scope 的 API Token；
2. 下载固定版本 SDK 和工具链；
3. 从示例生成 Guest App；
4. 编译 Wasm；
5. 使用 MicroPixel WAMR fork 固定 commit 生成 RISC-V 32-bit AOT format v6；
6. 生成并签名 `.mpxapp`；
7. 上传、安装、启动；
8. 订阅日志；
9. 执行输入与截图序列；
10. 修复、重建、升级和回滚。

每份响应都应包含机器可读错误码、request/job ID，以及 Agent 下一步可以采取的动作。

## 17. 本地与托管构建

### 17.1 本地可复现环境

发布一个锁定版本的容器或 devcontainer，包含：

- 与项目匹配的 WASI SDK/Clang；
- MicroPixel WAMR fork commit `482b17e07fc46e80ffd23e5290871d42c49748e7`，AOT format v6；
- `wasm32` C++23 编译参数；
- RISC-V 32-bit AOT target、ABI、CPU feature 参数；
- Bundle/Package 构建和签名工具；
- SDK headers、示例和 conformance check。

镜像以 digest 固定；SDK release manifest 给出所有工具版本和 SHA-256。CLI 建议提供：

```text
micropixel new
micropixel build
micropixel package
micropixel run [project]
micropixel run --no-follow
micropixel bundle validate <bundle>
micropixel auth pair --connection-code <code>
micropixel app install [project]
micropixel app install --start --follow
micropixel app start [app-id] --follow
micropixel logs --follow
micropixel input sequence <file> --screenshot <jpeg>
```

CLI 最终调用仓库现有 guest build 和 package 逻辑，避免维护第二套参数。
`build`、`package` 和 `app install` 默认使用当前目录；`app start` 省略 App ID 时从当前 `app.json` 推导。
启动后跟随日志复用同一 Guest log cursor，用户中断跟随不停止 App。旧的 `validate`、`auth login` 和
`app upload` 在 0.9.x 兼容期保留弃用别名。

`run` 是显式的开发部署编排，不使用文件监听：先在本地完成 development profile 的 package，再记录并
停止当前 Guest、安装、启动和跟随日志。package 失败不改变设备；安装或启动失败时 CLI 尽力恢复此前运行的
App。普通 `app install` 和 Host 仍保持“运行中拒绝安装”的安全语义，不隐式停止 App。

### 17.2 托管构建

托管 Build Worker 是独立低权限进程/容器：

- 输入只接受有大小上限的源码归档或 Git snapshot；
- 默认无外网、只读 SDK、临时工作目录；
- 限制 CPU、内存、磁盘、进程数和执行时间；
- 不挂载 Control 服务 secret 或数据库；
- 输出编译日志、SBOM/manifest、hash 和签名 package；
- 构建完成后上传 artifact，再由独立安装命令下发设备。

Control API 进程本身绝不直接执行用户 CMake、Clang 或脚本。

## 18. 安全与隐私

发布前必须满足：

- 设备严格验证服务端 TLS 证书链、用途和主机名；设备无可信 wall clock，不验证证书有效期；
- 设备 credential 不离开设备，服务端签名密钥使用轮换和 KMS/受限文件权限；
- 配对、Token、安装、输入、截图和日志分别授权；
- 所有命令记录审计事件，包含操作者、设备、scope、结果，不含 secret；
- artifact 使用短时签名 URL，并设置内容类型、大小和过期清理；
- Package 必须校验 hash、签名、ABI、大小和权限；
- 日志和系统信息默认脱敏，设置服务端保留期和删除接口；
- 网站启用 CSRF 防护、CSP、SameSite cookie、限流和登录保护；
- 设备 UI 能看到当前远程会话并能一键断开/撤销；
- 高风险操作可配置为首次或每次设备确认；
- 用户物理操作始终能终止远程输入。

威胁模型至少覆盖：连接码撞库、Token 泄露、重放命令、恶意 package、断电安装、日志泄密、截图隐私、远程输入劫持、服务端被入侵和设备时钟不可信。

## 19. 可靠性与资源约束

- 控制面不可导致 App 音频、输入或 UI 实时路径阻塞；
- 每个网络 payload 先校验长度，再解析；
- 截图、日志和 package 使用流式传输，不复制完整内容到多个缓冲区；
- 网络不可用不影响本地 App 和 System Settings；
- 服务器离线时连接码功能明确显示不可用，不生成只能本地看见的伪码；
- 设备没有可信 wall clock 时，以服务 challenge 和相对 deadline 防重放，在线后校时；
- 服务端命令结果至少保存到 Token/设备策略允许的期限；
- 所有 Job 可观测 queue time、execution time、bytes、retries 和最终错误；
- 内存不足、PSRAM 不可用、Flash 满、queue full 都返回稳定错误而不是重启设备。

## 20. 分阶段实施

### 20.1 当前落地状态（2026-08-25）

当前实现已经从骨架推进到可编译的只读诊断和首批运行控制：

- `control/apis` 使用 Fastify 实现无数据库 bootstrap、HS256 设备凭据、内存在线表、单次连接码、Console/API Token、设备与 Console 两条 NDJSON 长连接、设备事件入口、资源型 Job API、日志快照和文件 screenshot/package artifact；Console 长连接推送 presence、Job 和事件，并在 heartbeat 中主动结算超时；
- `control/site` 从同一份首页内容构建公开网站 `/`，并在 `/docs/*` 发布人类与机器文档；
- `control/console` 固定发布在 `/console/`。未登录或会话失效时只显示连接码、首页和文档入口；登录后使用单页 Overview 和按场景打开的对话框，不暴露 Console 会话凭据或期限。顶部可返回首页、主动退出并清除本地 Console 会话，也可签发默认包含全部设备权限的 7 天、30 天或永久 API Token；App 列表连接后自动同步，每项操作都有持续回执与可展开的完整结果；
- System Settings 已增加 Remote Control 页面，启用状态保存在 `sys_store/control`，设备身份也独立保存在该命名空间；
- Runtime 通过抽象 `GuestLogSink` 把 Guest `log_write` 复制到 PSRAM 中的 1024 条固定环形缓存（约 1.1 MiB），网络层不反向依赖 Remote Control；`logs.read` 使用绑定 App Session 的不透明 cursor，每个结果最多返回 48 条，由 Console 在日志对话框中根据 `hasMore` 连续追赶积压；服务端不调度轮询，设备不主动上传；
- Host 与网络 Agent 之间使用 PSRAM 后备的固定容量命令/结果队列；网络任务不直接调用 WAMR、LVGL 或驱动；
- 已实现 `screen.capture`、`app.start`、`app.stop` 和包含触摸、语义按键、延迟、带 ID 截图的 `input.sequence`；虚拟触摸经 Platform 坐标校验后重新进入 `SystemGestureRouter`，按键通过 Input 1.1 的固定键码事件进入 Guest；Remote Control 从当前显示 framebuffer 使用 ESP32-P4 JPEG 外设编码，网络任务再独立上传；
- Hall、System Settings、Remote Control 等 modal System UI 都会泵入安全的截图和输入命令；输入序列由
  跨 Host 循环的固定容量状态机推进，可以跨页面执行“输入→延迟→截图”，App 生命周期命令仍请求当前
  modal UI 安全退栈后再改变 Hall/Foreground 状态；
- Fastify 将 JPEG 保存为无数据库文件 artifact，并以 `screen:read` JWT 保护下载；截图保留一小时，每设备
  最多 8 项、进程最多 64 项，每分钟清理过期文件，启动时清理无法由内存索引恢复的孤儿截图；Console 能
  显示命令结果和最新截图；App 生命周期失败保留 Host 返回的具体稳定错误码；
- Fastify 上传入口校验 Bundle v1 header、section 边界/hash，计算 SHA-256 并以设备 UUID 隔离文件；设备只能用匹配的 Device credential 下载；
- ESP32-P4 分区是单一 24 MiB 可写 `app_store`；BundleFS 使用离散 64 KiB 数据块、写时复制和
  分区内四 Bank Catalog，所有 App 均可升级和卸载；
- 创建 Job 的 API 支持内存窗口内的 `Idempotency-Key` 去重与 1–300 秒命令 timeout；服务端离线排队过期，设备把剩余时间转换为 FreeRTOS 单调时钟 deadline，并在 Host 开始执行以及输入步骤之间复核；
- 设备为终态 `command.completed` 预留 PSRAM 优先的固定结果队列槽位；当前连接发送失败时缓存结果，控制流重连后由命令重投触发带新 session envelope 的完成状态重放。服务端按 `eventId/commandId` 去重，并把 `eventSequence` 作为会话内顺序和缺口观测；
- Fastify 对所有未完成命令按 replay policy 做进程内重连重投，设备以 boot-local 16 项 command ID 窗口抑制重复
  执行；服务重启和设备重启后的 exactly-once 仍未解决；
- 链接的 `esp-http3` 已支持 PSRAM allocator、大 POST 的 1 KiB 分块上传、ACK-only packet number 跟踪和
  RFC 9001 Key Update；旧 PNG 路径曾以 720×720、约 183 KiB artifact 验证跨服务端 Key Phase 上传，
  当前 JPEG 路径仍需补真机时延、画质和连续截图验收；
- `/sdk/releases/latest` 发布机器可读 SDK 版本、SHA-256 和下载地址，版本化 tar.gz 包含 Public SDK、ABI、Guest runtime、Demo 例程与正式 AOT/Bundle 构建脚本；Console 开发文档页提供 SDK、Agent 指南和 OpenAPI 入口；
- Control 从 JSONC 加载最新固件并发布内容寻址镜像；Web Console、App Hall 更新红点/快捷入口和 System Information 都可发起同一套 OTA。设备即使关闭 Remote Control 也会发现更新，安装前验证长度、SHA-256 和 ESP image 内嵌版本，再写入非活动 OTA 分区；
- 未实现的远程命令返回明确的 `not_implemented` 应用层结果，不能把 QUIC ACK 当成命令完成。

本轮自动验证基线为：Fastify 27 项测试、TypeScript typecheck、Control/Console production build、
Firmware Host 回归（含 App Store 与 BundleFS）、HTTP/3 TLS parser 16 项测试、Firmware 格式/架构检查、Guest 全量构建
（含 key input conformance）和完整 System Shell + 三个集成 Bundle 构建。真机正向验收覆盖严格
CA/主机名 HTTP/3、NVS 身份跨重启、一次性连接码、系统信息、App 列表、Hall→Settings→Remote 页面输入
序列、带 ID 截图以及大 artifact；不在文档或日志样例记录设备 UUID、MAC、连接码和 Token。JPEG 硬件
编码路径仍需补充连续截图和复杂 System UI 的真机回归。

尚未完成的运行控制边界也必须明确：Suspend/Resume、跨设备重启的持久结果恢复、持久 artifact manifest、
物理输入抢占和生产级审计仍待实现。输入序列会在成功、失败或过期时对仍按下的 contact 和 semantic key
注入 `cancel`。App Store 当前把完整
Bundle 暂存在 PSRAM，不含 `.mpxapp` 外层数字签名、版本/capability policy、手动回滚选择和真机断电
矩阵；安装时还会同步占用 Host 控制循环。因此 Phase 1/2 已有可运行子集，Phase 3 只有开发版最小事务，
仍不能宣称生产安全或 Agent 开发闭环已经完成。

当前链接的 `esp-http3` 已在代码层完成服务端证书链、证书用途/主机名、TLS 1.3
`CertificateVerify` 签名和 `Finished` MAC 验证，并用显式状态机约束 ServerHello、
EncryptedExtensions、Certificate、CertificateVerify、Finished 与 NewSessionTicket 的顺序和 QUIC
加密级别。固件必须配置 `MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64`。设备没有可信 wall clock，
产品配置关闭 `CONFIG_MBEDTLS_HAVE_TIME_DATE`，因此 X.509 `notBefore`/`notAfter` 不参与验证，TLS 连接也不
等待 SNTP。
`MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS` 只用于受信局域网排查，它额外跳过证书链和主机名，
仍强制证书用途、CertificateVerify 和 Finished。该实现已经通过 ESP32-P4 编译，但错误 CA、错误 SAN、
篡改 CertificateVerify/Finished 和 PSK 恢复仍需真机负向测试；严格 CA/主机名的正向真机连接已通过，
但 Phase 0 尚不能标记为完整安全验收。

可信配置使用单个 DER trust anchor，不链接系统根证书 bundle。ESP32-P4 产品配置关闭运行时不支持的
PSA 硬件 ECDSA verify driver，由软件 ECDSA 完成 TLS 1.3 CertificateVerify。

### Phase 0：协议和安全骨架

- 冻结 bootstrap、pairing、Device credential、Console/API Token、scope、command/event schema；
- TypeScript API 完成持久签名密钥、无状态 bootstrap、内存 Registry 和真实 JWT 签名；
- 设备端完成 TLS 验证、身份 NVS、credential 认证、长连接和重连；
- System Settings 增加 Remote Control 状态与启用开关。

验收：服务重启后设备自动重连；NVS 抹除后 UUID 变化；旧 Console/API Token 无法控制新身份；断网不影响本地使用。

### Phase 1：只读诊断

- 系统信息、App 列表、当前 App；
- `AppLogBroker`、日志 REST/SSE；
- 传输无关截图和 artifact 上传；
- Console Overview、Screen、Logs。

验收：长时间日志和连续截图无泄漏、无 UI 卡死，断线后 cursor 正确恢复。

### Phase 2：运行控制与输入

- Start/Stop/Suspend/Resume 命令；
- 统一 InputInjection；
- 输入/延迟/截图序列和 Job 结果；
- 高风险 scope、设备状态图标和审计。

验收：物理输入可抢占；取消时无残留 touch-down；重复命令不重复执行。

### Phase 3：App Store

- 已完成：统一 24 MiB 可写分区、PSRAM staging、SHA-256、BundleFS 四 Bank Catalog、安装/升级/卸载；
- 待完成：流式 staging、显式回滚、分页 UI 和逐断电点真机测试；
- 待完成：正式 `.mpxapp` manifest、数字签名、版本和 capability policy。

验收：在下载、写入、提交各阶段断电，启动后始终得到旧版本或完整新版本，不出现半安装状态。

### Phase 4：Agent 开发闭环

- SDK release、OpenAPI、Agent 文档、CLI；
- 锁定版本的本地构建容器；
- 隔离 Build Worker；
- 示例自动化：build → package → install → run → input → screenshot/log → result。

验收：一个没有项目上下文的 Agent 只读取公开文档即可完成 Demo App 的构建、安装和自动化运行。

## 21. 测试矩阵

至少增加以下测试：

- 身份：首次 bootstrap、服务重启、NVS 抹除、auth epoch、credential 损坏恢复；
- 配对：过期、重复消费、并发提交、错误次数、限流、取消；
- Token：typ、scope、设备隔离、expiry、epoch 不匹配、服务端签名密钥轮换；
- 协议：乱序、重复、断线重放、deadline、未知版本、超大 payload；
- App：一 App 限制、启停竞争、force stop、安装中启动、运行中卸载；
- 输入：步骤上限、坐标边界、取消补 up、物理抢占、受保护 UI；
- 截图：并发、PSRAM 不足、LVGL timeout、上传断线、hash 校验；
- 日志：环形覆盖、cursor、dropped、超长消息、App session 隔离；
- 安装：签名错误、hash 错误、ABI 不兼容、Flash 满和每个断电点；
- 安全：TLS 失败、重放、跨设备 Token、artifact URL 过期、日志脱敏；
- 资源：24 小时长连接、反复重连、截图/日志/安装压力和 heap/PSRAM 水位。

固件相关变更继续运行 Host test、格式检查和 P4 baseline；Bundle/SDK 变更运行 Guest、conformance、正式 Bundle 和 System Shell 集成构建。涉及网络、触摸、截图和断电恢复的行为必须补充 ESP32-P4 真机验收。

## 22. 建议默认决策

如果没有额外产品约束，建议首版采用以下默认值：

- 菜单名：Remote Control；
- 生产固件默认关闭，开发固件可 Kconfig 默认开启；
- 连接码：8 位 Crockford Base32，5 分钟、单次使用、最多 5 次失败；
- Console Token：1 小时；API Token：由 Console 选择 7 天、30 天或永久；
- 设备身份：服务端生成 UUIDv4 + 长期设备 JWT + auth epoch；
- 服务端状态：单 Fastify 实例、内存 Registry、文件 artifact、无数据库；
- 控制协议：HTTP/3 + 版本化 NDJSON，artifact 独立 stream；
- Web 实时更新：Console Token 认证的版本化 NDJSON 长连接，交互请求使用普通 REST；
- 远程输入和 App 安装必须是独立高风险 scope；
- 截图 URL 短时有效，Guest 日志默认短期保留；
- 所有 App 都通过 BundleFS 安装；更新写入新的离散数据块后原子切换 Catalog，不原地覆盖 active 数据块；
- App Package 使用 SHA-256 和数字签名，FNV 只保留作内部快速完整性检查；
- 所有改变设备状态的 API 返回 Job，并支持 idempotency key。

这些默认值能先形成安全、可调试、可自动化的完整闭环，同时保留以后迁移二进制协议、多实例服务和正式 App 发布签名体系的空间。
