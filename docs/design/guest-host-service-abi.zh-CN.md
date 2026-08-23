# Guest–Host Service ABI v1 最终设计

## 决策

MicroPixel v1 使用“极小稳定 Core ABI + 可版本化 Service”的边界。Public Guest 永久面对少量
传输原语，业务能力通过 Service method、submit channel 和 event 演进。代码尚未发布，因此
直接移除早期一功能一 import 的接口，不承担 legacy shim 和双轨维护成本。

```text
Typed C++ Guest SDK
        |
        |  open once / cached handle
        v
7 Core imports: log, clock, wait, open, call, submit, version
        |
        v
fixed-capacity ServiceRegistry
        |
        +-- TimerServiceEndpoint    -> TimerService
        +-- StorageServiceEndpoint  -> StorageService
        +-- ResourceServiceEndpoint -> ResourceService
        +-- RandomServiceEndpoint   -> device::RandomService -> platform hardware RNG
        +-- GraphicsServiceEndpoint -> device::GraphicsService
        +-- InputServiceEndpoint    -> device::InputService
        `-- AudioServiceEndpoint    -> device::AudioService
                                           |
                                           v
                                  selected platform backend
```

Service Endpoint 负责 wire schema 和 method/channel 路由；领域 Service 负责生命周期、并发、配额
和业务规则；Platform backend 只负责硬件。这样不会把所有逻辑塞进一个大 switch，也不需要在领域
代码里散布板型 `#if`。

## Core ABI

Core v1 只有七个 import：

1. `abi_version`
2. `log_write`
3. `clock_now`
4. `event_wait`
5. `service_open`
6. `service_call`
7. `service_submit`

`service_open` 是唯一的 Service ID/版本查找路径，并返回 Guest-local handle。Registry 使用固定数组，
每个 Guest 构造时缓存 Service descriptor；call/submit 不做字符串查找、堆分配或设备信息查询。
Registry 的可变参数构造函数从 `GuestContext` 中唯一的显式 endpoint 列表推导实际数量；新增 Service
不需要再同步修改 Registry 构造签名或固定服务计数。

`service_call` 用于低频控制和有界响应。`service_submit` 用于 Graphics CommandBuffer，以及未来可能的
Audio/Network 批量数据。两者分开可让 Host 对实时数据采用不同的大小限制、队列、背压和指标，而无须
扩张 Core ABI。

## Service 和版本

| Service | ID | v1 传输 |
|---|---:|---|
| Timer | 1 | call + event |
| Storage | 2 | call；value 是 opaque bytes，typed value 由 SDK 编解码 |
| Resource | 3 | v1.2 call + event；load Bitmap、创建/局部更新 offscreen surface、原子 update frame |
| Random | 4 | call(get_u32) |
| Graphics | 16 | call(get_info) + submit(command batch) |
| Input | 17 | call(get_info) + event |
| Audio | 18 | call；后续 stream 可增加 submit channel |
| Network | 19 | 仅预留；后续 method/submit/event 方式加入 |

Core 和每个 Service 各自维护 major/minor。major 表示不兼容 wire 变化；minor 只能追加 method、
channel、event、capability 或结构尾字段。`service_open` 要求 major 相同且 Host minor 不低于 Guest
最低要求。数字 ID 一旦发布不得复用。

## Wire 与所有权

- 所有可演进结构都以 `uint16_t size` 开头，保留字段发送时清零。
- 所有 multi-byte 数值使用 Wasm/Host 共同的小端表示；不传 C++ 对象、vtable、Host 指针或 STL 类型。
- `service_call` 单独返回实际/所需 response size；BUFFER_TOO_SMALL 可安全重试。
- Timer、request、bitmap 等是 Guest-local opaque handle；SDK 用 move-only RAII 管理释放。
- Offscreen surface 与只读资源 Bitmap 共用 handle/配额模型，但只有 Host 标记为 mutable 的 PSRAM
  backing store 接受局部 update；Guest 从不获得 backing pointer。多个 update 可由 begin/commit 包成一个
  原子帧，Host 在 commit 前不向显示任务发布中间 damage。
- Storage wire 只处理 key + opaque value；u32/bool 等类型属于 SDK，避免为每种值扩张 ABI。
- Host runtime 的队列容量、对象池容量和设备资源数不属于 ABI；Guest 通过 service info/capability 查询。

## 事件

`micropixel_event_t` 固定 48 bytes：size、event_id、service_id、flags、source、timestamp、sequence、
status 和 16-byte payload。事件按 `service_id + event_id` 解码，允许不同 Service 独立扩展。

`event_wait(..., timeout_us)` 支持立即轮询、有限超时和 `UINT64_MAX` 无限等待，分别返回 OK、TIMEOUT
或 CLOSED。Timer periodic event 和 Touch move 可按资源合并；Resource completion 等必达事件使用
背压，不能静默丢弃。

## 性能约束

- production Guest profile 使用 Clang `-O2`，WAMR AOT 使用 opt level 3；`size` profile 才使用 `-Oz`。
- Graphics 对 HUD/对象树保持批量 CommandBuffer；高频像素层通过 Resource 1.2 的 bounded dirty-rect
  update 修改已保留 Bitmap，并用 update frame 将一个逻辑 tick 的全部 damage 原子发布，不要求重交整帧命令。
- Service descriptor、handle 和 Graphics info 在 Guest 生命周期内缓存。
- Host 热路径不得分配堆内存，不得按 service ID 线性搜索，不得重复调用设备 `GetInfo()`。
- 每次 ABI 修改必须至少比较 Snake 的 Wasm/AOT 大小、imports、固件 release 构建；真机发布门禁还应测
  frame time、submit latency、touch-to-submit latency、audio underrun 和 heap high-water。

## 演进门槛

优先级固定为：新增 method/channel/event → 新增 Service → 最后才是新增 Core import。只有现有
open/call/submit/event 模型在真机基准上无法达到吞吐、时延或背压要求时，才允许增加专属 import。
任何新增 Core import 都必须同步更新 ABI header、Host native 注册、Guest allowlist、SDK lowering、
pointer/length 负向测试和版本兼容测试。
