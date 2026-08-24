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
- Renderer Frame command stream 使用 `service_submit`，避免逐条绘图跨 ABI。
- 未来 Audio/Network 流量先使用 `service_submit` 的独立 channel；只有真机基准证明该传输
  无法满足背压或吞吐需求时，才讨论新增 Core import。
- Timer、Input 等真正的异步通知通过 `micropixel_event_t` 返回；v1 Resource 加载是同步 call。

当前 Graphics command protocol 包含 `PUSH_STATE` / `POP_STATE`、`BLEND_RECT`、`DRAW_TEXTURE` 和
`BLEND_TEXTURE`。SDK 用前两者 lowering `Save`、clip、translation 和 `Restore`；capable Host 可把稳定
scope 识别为 retained translation，但该优化不进入 Public C++ API。texture command 的 `opacity` 与
资源自身逐像素 alpha 相乘；不透明 `DRAW_TEXTURE` 走 Host copy 快速路径。Texture wire command 分别携带
destination rectangle 与 source rectangle，因此 1:1、裁剪、缩放及裁剪后缩放不需要新增 opcode。
`max_draw_operations` 是 Public SDK 可见的稳定绘制预算，`max_frame_commands` 只用于 SDK/Host transport，
自动生成的 state 与跨批续接记录不会改变应用预算。

事件 envelope 固定为 48 bytes，包含 `service_id + event_id`、flags、source、Guest 单调时间、
sequence、status 和 16-byte payload。event ID 只在所属 Service 内解释；当前定义 Timer expired、
Input touch 和 Core host wake。新增事件不会扩大 Core import 表。

- 周期 Timer 队列中同一 handle 最多保留一条记录。积压时 `elapsed_us` 累加，`missed_count` 统计未单独
  投递的 tick；队列满导致的 tick 也结转到下一次成功事件。
- Touch wire 坐标是 `int32_t`。`pressure_per_mille` 仅在 Input 宣告
  `MICROPIXEL_INPUT_CAP_PRESSURE` 时有意义，范围固定为 0..1000。GT911 不宣告该能力并始终写 0。

## 稳定性与安全规则

- 已发布的 Service、method、channel、event、field、capability 和 opcode ID 永不改义、永不复用。
- Core ABI major 不兼容时拒绝加载；Service major 必须相同，Host minor 必须不低于 Guest 要求。
- Host 在进入 Service Handler 前完成 Guest pointer/length 校验；Handler 仍验证 schema、上限和句柄。
- Guest 不持有 Host 指针。Timer、Texture 等资源使用 Guest-local generation handle，
  由 SDK 的 move-only RAII 对象释放。
- Retained scene 持有独立 Texture 引用。Guest release 只撤销 Guest 引用；显示场景替换或 Session teardown
  后才撤销 scene 引用，两个引用都归零时才释放像素内存。
- Resource 1.0 提供同步 `LOAD_TEXTURE`、release、streaming texture create/update 和 update batch。
  压缩图片仍由 Host worker 解码，但 `service_call` 等待 worker 完成并在等待期间暂停 Guest watchdog；
  不分配 request handle，也不产生 Resource-ready event。StreamingTexture 像素由 Host PSRAM 持有并计入
  Texture 配额；update request 由 32-byte header 和紧密排列的原生格式脏矩形组成，总长度不超过
  4096 bytes。Host 只允许修改带 `MICROPIXEL_TEXTURE_FLAG_STREAMING` 的句柄，并再次校验 bounds、
  pitch、格式和精确 payload 长度。Batch 内先合并 damage，finish 时统一 invalidate 并只唤醒一次 compositor。
- 固定容量、配额、超时和背压属于 Host 策略；只有 wire-format 硬上限进入公共 ABI。
- Public Guest 链接必须使用 allowlist，禁止用全局 `--allow-undefined` 掩盖拼写错误或未授权依赖。

新增 import 必须同时给出无法用现有 Core/Service transport 表达的真机性能证据，并更新 C ABI、
Host 注册、Guest lowering、allowlist、内存安全负向测试和兼容性 fixture。
