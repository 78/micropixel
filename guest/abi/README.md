# MicroPixel Guest–Host ABI v1

状态：**v1 已实现，项目尚未发布。** 机器可读定义以
[`micropixel_abi.h`](micropixel_abi.h) 为准；[`allowed_imports.txt`](allowed_imports.txt)
是 Public Guest 唯一允许的 Host import 清单。

普通应用只使用 `guest/sdk/` 的强类型 C++ API。`guest/runtime/sdk.cpp` 负责 Service 打开、
wire struct 编解码、句柄所有权和错误转换，应用不直接依赖 C ABI。

## 七个稳定入口

| import | 用途 |
|---|---|
| `abi_version() -> i32` | 返回 Core ABI 的 `major << 16 | minor` |
| `log_write(level, bytes, length) -> i32` | 有界 UTF-8 日志 |
| `clock_now() -> i64` | Guest 启动后的单调微秒时间 |
| `event_wait(event, capacity, timeout_us) -> i32` | 等待统一的 48-byte Service Event；`UINT64_MAX` 表示无限等待 |
| `service_open(service_id, required_version, info, capacity) -> i32` | 协商 Service 版本并取得 Guest-local 稳定句柄 |
| `service_call(handle, method, request, request_size, response, response_capacity, response_size) -> i32` | 小型、有界、同步控制请求 |
| `service_submit(handle, channel, bytes, length) -> i32` | 高频或批量数据提交 |

v1 不注册按功能增长的专属 import，也不保留未发布的旧 ABI shim。新增 Timer、Storage、
Audio 或 Network 方法通常只新增 Service 内的稳定数字 ID 和 wire schema，不增加 import。

## Service 模型

当前 Service ID：Timer `1`、Storage `2`、Resource `3`、Graphics `16`、Input `17`、
Audio `18`；Network `19` 只预留 ID，尚未实现。

`service_open` 校验 Service 独立的 major/minor，返回 48-byte `micropixel_service_info_t`：

- `handle` 只在当前 Guest 实例内有效；
- `flags` 指明 `call`、`submit`、`events` 能力；
- `capabilities` 表示可选功能；
- `max_request_bytes`、`max_response_bytes`、`max_submit_bytes` 给出传输上限。

SDK 每个 Service 只打开一次并缓存句柄和不可变信息。Host 的固定容量 Registry 在打开时做
ID/版本查找；后续 call/submit 只做句柄边界检查、数组索引和一次 Handler 调用，不分配堆内存，
也不在 Graphics 每帧提交时重复查询设备信息。

`service_call` 的 `response_size` 始终由 Host 写入。响应缓冲区不足时返回
`BUFFER_TOO_SMALL` 并报告所需大小。所有可演进 request/response 结构以 `uint16_t size`
开头；接收端接受已知前缀，新增字段只能追加并默认按零解释。

## 控制面、提交面和事件面

- Timer、Storage、Resource、Audio 控制以及各类 `get_info` 使用 `service_call`。
- Graphics CommandBuffer 使用 `service_submit`，避免逐条绘图跨 ABI。
- 未来 Audio/Network 流量先使用 `service_submit` 的独立 channel；只有真机基准证明该传输
  无法满足背压或吞吐需求时，才讨论新增 Core import。
- 异步完成统一通过 `micropixel_event_t` 返回。

当前 Graphics command protocol 直接包含可选 Surface translation、`BLEND_RECT` 和
`BLEND_BITMAP`，不保留尚未发布过的中间 minor 版本。后者的 `opacity` 是当前 bitmap command 的统一
alpha，和资源自身的逐像素 alpha 相乘，不是跨 command 保留的渲染状态。不透明 `DRAW_BITMAP` 走
Host 的 copy 快速路径。

事件 envelope 固定为 48 bytes，包含 `service_id + event_id`、flags、source、Guest 单调时间、
sequence、status 和 16-byte payload。event ID 只在所属 Service 内解释；当前定义 Timer expired、
Input touch、Resource ready 和 Core host wake。新增事件不会扩大 Core import 表。

## 稳定性与安全规则

- 已发布的 Service、method、channel、event、field、capability 和 opcode ID 永不改义、永不复用。
- Core ABI major 不兼容时拒绝加载；Service major 必须相同，Host minor 必须不低于 Guest 要求。
- Host 在进入 Service Handler 前完成 Guest pointer/length 校验；Handler 仍验证 schema、上限和句柄。
- Guest 不持有 Host 指针。Timer、LoadRequest、Bitmap 等资源使用 Guest-local generation handle，
  由 SDK 的 move-only RAII 对象释放。
- Resource 1.1 在既有 Bitmap handle 空间中追加 offscreen surface create/update；Resource 1.2 再追加
  update-frame begin/commit，用于把多次局部写入组成一个原子显示事务。Surface 像素由 Host
  PSRAM 持有并计入 Bitmap 配额；update request 由 32-byte header 和紧密排列的原生格式脏矩形组成，
  总长度不超过 4096 bytes。Host 只允许修改带 `MICROPIXEL_BITMAP_FLAG_MUTABLE` 的句柄，并再次校验
  bounds、stride、格式和精确 payload 长度。Frame 内的写入先按 backing Bitmap 合并 damage；commit
  才在同一个显示锁临界区 invalidate 全部区域并唤醒一次 compositor。
- 固定容量、配额、超时和背压属于 Host 策略；只有 wire-format 硬上限进入公共 ABI。
- Public Guest 链接必须使用 allowlist，禁止用全局 `--allow-undefined` 掩盖拼写错误或未授权依赖。

新增 import 必须同时给出无法用现有 Core/Service transport 表达的真机性能证据，并更新 C ABI、
Host 注册、Guest lowering、allowlist、内存安全负向测试和兼容性 fixture。
