# MicroPixel Guest–Host ABI v2

状态：**v2 已实现，项目尚未发布。** 机器可读定义以
[`micropixel_abi.h`](micropixel_abi.h) 为准；[`allowed_imports.txt`](allowed_imports.txt)
是 Public Guest 唯一允许的 Host import 清单。

普通应用只使用 `guest/sdk/` 的强类型 C++ API。`guest/runtime/` 负责 Service 打开、
wire struct 编解码、句柄所有权和错误转换，应用不直接依赖 C ABI。

Core ABI 为 **2.0**，与 1.x 完全不兼容，不保留任何 1.x 方法编号、结构前缀或兼容 shim。
所有 Service 接口在 2.0 下重新从 **1.0** 起算：Graphics、Input、Resource、Audio、System、Timer、
Storage、Random、Devices、Sensors、GPIO、Haptics、Power 均为 1.0。绘制通道为 `SCENE`（1）与
`RASTER`（2）；2.5D 与多边形游戏的几何前端（`sdk/raycast.hpp`、`sdk/mesh_renderer.hpp`）完全在
Guest SDK 内，不引入新的 wire。
协议尚处于正式版前迁移，冻结状态见 [SDK API 设计](../../docs/design/sdk-api.zh-CN.md)。

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

不注册按功能增长的专属 import。新增 Timer、Storage、Audio 或 Network 方法通常只新增 Service
内的稳定数字 ID 和 wire schema，不增加 import。

## 命名规则

### 句柄与字段

类型明确的资源句柄字段使用 `texture_handle`、`font_handle`、`surface_handle`、`sensor_handle`、
`gpio_handle`、`haptics_handle`、`timer_handle`、`clip_handle`、`playback_handle` 和 `stream_handle`。
通用句柄请求／响应保留 `handle`；`texture_slot` 是 Raster 槽编号，`node_id` 等是标识符，
`buffer_index` 是数组索引。SDK 的 Texture、Font 等资源对象不加 handle 后缀。

普通资源句柄 0 无效；SpriteBatch 的 `texture_handle = 0` 专门表示使用实例颜色绘制纯色矩形。
Raster 的 `texture_slot = 0` 是有效槽位，不能与普通纹理句柄混用。

所有资源句柄（含 `font_handle`）都是 32 位；`device_id` 也是 32 位。带 `size` 的结构以
`uint16_t size` 开头，填充字段按出现顺序命名 `reserved0`、`reserved1`…（数组同样带编号），Host
拒绝非零 reserved。指向 Guest 线性内存的字段按内容命名（`pixels`、`entries`），其字节数一律叫
`length`，行距叫 `pitch`；源矩形一律是 `source_x/source_y/source_width/source_height`，目标矩形是
`x/y/width/height`；Direct Surface 的 buffer 序号在 create、present、raster header 和 event 中都叫
`buffer_index`。上限字段用 `max_`/`min_` 前缀（`max_duration_ms`、`min_interval_us`），千分比用
`_per_mille`，时间用 `_us`/`_ms` 后缀，`capabilities` 在具体 Service 的 info 中为 32 位、在
Service/Device 目录中为 64 位。颜色按格式命名：Scene 用 `rgb888`，Raster 记录的 `color` 是 canonical
RGB565；不透明度一律叫 `opacity`（0..255）。GET_INFO 返回值命名为 `*_info_t`，其他响应为
`*_response_t`，请求为 `*_request_t`。

### Method

带句柄的 method 动词成对且固定含义：

| 动词对 | 含义 | 示例 |
|---|---|---|
| `LOAD` / `UNLOAD` | 从 Bundle 资源建立／撤销 Guest 引用 | `TEXTURE_LOAD`、`FONT_LOAD`、`CLIP_LOAD` |
| `CREATE` / `DESTROY` | Host 按参数构造／销毁对象 | Timer `CREATE`、`SURFACE_CREATE`、`DYNAMIC_TEXTURE_CREATE` |
| `OPEN` / `CLOSE` | 取得／归还既有设备或流的 Session 内 lease | `PCM_STREAM_OPEN`、Sensors/GPIO/Haptics 的 `OPEN`/`CLOSE` |
| `START` / `STOP`、`PAUSE` / `RESUME` | 运行状态切换 | Timer `START`、`PLAYBACK_START`/`STOP` |

一个 Service 发放多种句柄时以名词前缀区分（`TEXTURE_LOAD`、`PCM_STREAM_OPEN`）；只发放一种句柄的
Service（Timer、Sensors、GPIO、Haptics）直接用动词（`CREATE`、`START`、`OPEN`、`READ`、`CLOSE`）。
动态纹理快照与 Bundle 纹理共用 `TEXTURE_UNLOAD` 释放。没有句柄的服务级 method 保留动词开头：
`GET_INFO`、`LIST`、`MEASURE_TEXT`、`STOP_ALL`、`GET_LOCALE`、`GET_LAUNCH_ARGUMENTS`。

## Service 模型

当前 Service ID：Timer `1`、Storage `2`、Resource `3`、Random `4`、System `5`、Devices `6`、
Graphics `16`、Input `17`、Audio `18`、Sensors `20`、GPIO `21`、Haptics `22`、Power `23`；
Network `19` 只预留 ID，尚未实现。

System 提供最长 31 bytes 的 BCP 47 locale tag。Host 返回当前 Catalog 和字体确实可用的 effective
Locale；基础固件只有 `en`，以后安装语言组件并改变设备语言不需要修改 Guest ABI。Locale 在一个
AppSession 内不可变；用户返回 App Hall 修改语言后，下一次启动 Guest 才会取得新值，不投递
locale-change event。`GET_LAUNCH_ARGUMENTS` 返回本次 AppSession 的只读参数快照：最多 16 项，UTF-8
字符串区合计最多 512 bytes，每项以 NUL 结尾并由固定 `offsets[]` 定位。Host 在进入 Guest `main()`
前完成边界校验和复制；Hall 启动返回空列表，暂停/恢复同一 Session 时快照保持不变。

`service_open` 校验 Service 独立的 major/minor，返回 48-byte `micropixel_service_info_t`：

- `handle` 只在当前 Guest 实例内有效；
- `flags` 指明 `call`、`submit`、`events` 能力；
- `capabilities` 表示可选功能；
- `max_request_bytes`、`max_response_bytes`、`max_submit_bytes` 给出传输上限。

SDK 每个 Service 只打开一次并缓存句柄和不可变信息。Host 的固定容量 Registry 在打开时做
ID/版本查找；后续 call/submit 只做句柄边界检查、数组索引和一次 Handler 调用，不分配堆内存，
也不在 Graphics 每帧提交时重复查询设备信息。

`service_call` 的 `response_size` 始终由 Host 写入。响应缓冲区不足时返回
`BUFFER_TOO_SMALL` 并报告所需大小。带 `size` 的结构不自动支持扩展：固定布局请求要求精确大小，
新增语义使用新 method 或 opcode。GET_INFO 家族的响应在当前 minor 下要求完整结构；以后的 minor
只能在尾部追加字段并明确缺失字段的默认值，Host 届时再接受旧前缀。Guest 使用新功能前须协商
所需版本并检查 capability，reserved 字段必须写零。冻结前的布局调整须同步 Host、Guest 与协议测试。

版本号仅由 Service 协商返回；Scene 消息头、Raster 消息头、各类查询和纹理／字体／音频加载响应
都不重复携带版本。纹理响应描述资源尺寸、格式、标志和句柄，不暴露 Host 像素存储的 stride。
Graphics GET_INFO 为 36 字节，Audio GET_INFO 为 28 字节，Audio Tone 请求为 20 字节。

## 控制面、提交面和事件面

- Timer、Storage、Resource、Audio 控制以及各类 `GET_INFO` 使用 `service_call`。
- Graphics Scene keyframe/patch 与 Raster draw list 使用 `service_submit`，避免逐对象调用跨 ABI。
- 未来网络下载的数据面优先使用独立 Service channel；只有真机基准证明现有 transport
  无法满足背压或吞吐需求时，才讨论新增 Core import。
- Timer、Input 等真正的异步通知通过 `micropixel_event_t` 返回；Resource 加载是同步 call。

## Graphics

### Scene 协议

Graphics wire 是 retained Scene 协议。首个提交发送完整 keyframe，之后仅发送 Container、Sprite、
SpriteBatch instance、Shape 或 Label 的属性差量；消息携带 generation、base revision 和
revision，Host 在本次提交期间容量固定的 scratch scene 中完成整包验证后再原子交换。SpriteBatch 可让蛇身、方块、
爆炸和粒子共享一个 Host 节点，patch 只携带变化的 instance。Texture 节点同时携带 destination 与
source rectangle，opacity 与资源自身逐像素 alpha 相乘；不透明复制、缩放和填充分别映射到
DMA2D/PPA 快速路径。节点、容器和 Batch instance 没有计数上限：wire id 为 uint16，
`node_count + batch_instance_count <= 65535`（每个 Batch 仍计一个节点），其余由 Host 内存决定。
其它容量是 Host 策略而不是 ABI 常量：`graphics_info` 返回 `max_text_bytes`、`max_scene_bytes`、
`max_raster_bytes` 与 `max_surface_buffers`，Guest Runtime 取 Host 值与自身静态容量的较小者作为有效上限。
产品 profile 当前为 1024 字节文本、128 KiB Scene、32 KiB Raster、3 个 buffer。Label 文本在两端都
存放在按需增长的 text arena 中，节点记录只保存偏移。Host 不按上限预分配，按提交需求显式扩容 PSRAM
数组，所有新缓冲申请成功后才迁移旧场景和合成状态。OOM 拒绝提交且保留旧场景；绘制期间不分配，
挂起保留，App 结束释放。

Scene record opcode 连续编号：`BACKGROUND`(1)、`CONTAINER`(2)、`NODE_LINK`(3)、`RECT`(4)、
`ROUNDED_RECT`(5)、`TEXTURE`(6)、`TEXT`(7)、`SPRITE_BATCH`(8)、`BATCH_INSTANCES`(9)。节点 property 位
`GEOMETRY`、`CONTENT`、`APPEARANCE`、`VISIBILITY`、`KIND` 占 bit 0..4，没有保留空洞。

`GET_INFO` 携带物理显示像素单位的 `safe_inset_{top,right,bottom,left}`。Host 从 Board 的显示几何
元数据填充这些值；Guest SDK 用向外取整换算到逻辑坐标，避免缩放后重新暴露一个被圆角、盖板或异形
边缘遮挡的物理像素。零值表示对应边没有额外安全内缩，而不是让 App 猜测板型。

`CONTAINER` 和 `NODE_LINK` record 建立真正的 retained tree。Container ID `0` 是 Scene 隐式根，
`1..container_count` 是连续 wire ID；每个 drawable 在 keyframe 中必须有一个 `NODE_LINK`，声明
parent container 和同级顺序。Container 可嵌套，translation、clip、opacity 和 visibility 沿父链继承，
drawable geometry、clip 和 translation 都是相对直接 parent 的局部物理坐标，Host 在遍历父链时合成最终
Scene 坐标；Scene root 隐含最终 viewport clip，drawable 和显式 clip 的局部矩形可以越过 parent 或 root，
Host 只栅格化祖先 clip 与 viewport 的最终交集；z-order 与 sibling order 决定树内绘制顺序。Scene touch
坐标不进入 Graphics wire，Guest SDK 负责高层控件的 Scene/local 转换。

`ROUNDED_RECT` drawable 携带填充色、描边色、圆角半径、描边宽度和整体 opacity。它不拥有独立像素
缓冲；Host 在 App Surface 的 damage 区域内直接栅格化。半透明颜色在绘制时与既有 RGB565 像素混合，
Surface 本身仍保持不透明 RGB565。半径和描边宽度超过短边一半时按短边一半截断。

`CONTAINER` record 的 `flags` 由 `CONTAINER_FLAGS` property 位携带。目前唯一的 flag 是 `CACHED_CONTENT`：Guest
声明该子树的内容变化远少于平移变化（滚动地图、tile 层），Host 可以把子树按 container 局部坐标栅格化到
保留缓存，之后每次平移只从缓存复制，不再重放子树；内容变化仍按局部矩形重绘缓存。缓存按不透明层
合成，未被后代覆盖的像素显示 Scene 背景色，因此绘制顺序在它之下的节点不会透出。flag 只是提示，不改变
绘制结果；Host 也可以忽略它。当前 Host 用它选择 Layer 快照容器：第一个 `parent_container_id == 0` 且带
该 flag 的 container 使用像素缓存；没有该 flag 时不启用容器缓存。校验规则：keyframe 的 container mask
必须包含 `CONTAINER_FLAGS`；未声明该 property 的 patch 必须回显当前值，与 `sibling_order` 相同；未知
flag 位被拒绝。

### Direct Surface

Direct Surface 是面向全屏软件渲染（raycaster、伪 3D、模拟器）的整帧呈现路径，与 retained Scene
互斥使用。它不新增 Core import，只有三个 method 和一个 event：

- `SURFACE_CREATE {width, height, pixel_format, buffer_count 1..3, flags}` 返回 surface_handle、
  `native_pixel_format`、`native_flags` 和 `max_full_frame_fps`。当前 `pixel_format` 只接受 `RGB565`；一个
  Session 同时最多一个 Direct Surface。`flags=0`（默认）是 **Host buffer**：Host 在 PSRAM 分配
  `buffer_count` 个 buffer 尺寸、面板字节序的 buffer，Guest 从不映射它们，只能通过
  `CHANNEL_RASTER` 记录按 buffer index 绘制。`GUEST_BUFFERS` 则由 Guest 在线性内存里自备 buffer 并按地址
  present；因为 Host 在 buffer 在飞期间持有指向 Guest 内存的指针，Bundle 必须声明 `pinned_memory`，否则
  create 返回 `UNSUPPORTED`。两种 buffer 的 `width/height` 都是 **buffer 尺寸**：等于 `GET_INFO` 的物理
  尺寸，或物理尺寸在两个轴上除以同一个整数（present 时带 `SCALE_NEAREST`，Host 放大）；
- `SURFACE_PRESENT {surface_handle, buffer_index, pixels, length, pitch, source_width, source_height, flags}`。Host
  buffer 下 `pixels/length` 为 0、`pitch = source_width * 2`、`source_width/source_height` 等于 buffer 尺寸，只有
  `buffer_index` 有意义。`GUEST_BUFFERS` 下 `pixels` 是 Guest 线性内存偏移，Host 校验
  `pixels..pixels+length` 完整落在当前线性内存内、按 `MICROPIXEL_SURFACE_BUFFER_ALIGNMENT`（64 B）对齐、
  `pitch >= source_width * 2` 且 `pitch * source_height <= length`。两种模式下 `buffer_index < buffer_count` 且该
  buffer 不在飞。任何失败返回 `INVALID_ARGUMENT`/`STALE_STATE`，不 trap。present 成功后该 buffer 归
  Host，直到 `GRAPHICS_EVENT_SURFACE_RELEASED`（payload `{surface_handle, buffer_index, timestamp_us}`）把它归还；
  Guest 在收到前不得改写。独占扫描输出的 Host 会保留当前显示的 buffer 直到下一帧替换它或 surface 销毁，
  所以 `buffer_count=1` 每帧都要等待，流畅渲染至少用 2。present 的像素**始终**是 `native_flags` 声明的面板
  字节序：Host buffer 由 Host 保持该序，`GUEST_BUFFERS` 的 Guest 看到 `RGB565_BYTE_SWAPPED` 时自己换序写入，
  wire 上不再有字节序 flag。`SCALE_NEAREST` 允许两种 buffer 的 `source_width/source_height` 为物理尺寸的
  整数分之一，Host 用最近邻放大；
- `SURFACE_DESTROY {handle}` 阻塞到在飞 buffer 全部归还，之后 handle 失效；destroy 本身即视为归还，
  期间不再投递 `RELEASED`；destroy 后 present 返回 `NOT_FOUND`。Session 暂停时 Host 停止扫描输出并归还
  在飞 buffer；App 退出时 Host 释放 surface。
- `GET_INFO` 的 `native_pixel_format`、`native_flags`、`max_full_frame_fps` 描述面板：`native_flags` 的
  `RGB565_BYTE_SWAPPED` 表示面板要求高字节先发，`DIRECT_SCANOUT` 表示 Host 有零拷贝扫描输出路径；没有该位
  时 present 仍然正确，只是经 App Surface 合成。

Host 语义：Direct Surface 处于独占扫描输出时，系统 UI（Status Layer、系统手势、过渡动画）一旦可见，Host
退出独占并临时把 present 回落到 App Surface 拷贝路径，隐藏后恢复；Guest 无需感知，只按 `RELEASED` 节奏
复用 buffer。

### Raster kernels

Guest 保留几何（光线投射、地板行、billboard 排序与深度测试），把逐像素贴图循环交给 Host 在 Guest task
上原生执行，目标是 Host buffer 模式的 Direct Surface，Guest 不接触任何像素。Raster 使用 16 位尺寸和
可复用的 8 位纹理槽编号。它不新增 Core import，提供三个 upload method 和一个 submit channel：

- Service descriptor 的 `capabilities` 含 `MICROPIXEL_GRAPHICS_CAP_RASTER` 时可用；
  光照档位上限由 `MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS` 定义，不另设查询字段；
  含 `MICROPIXEL_GRAPHICS_CAP_RASTER_POLYGON`（`1U << 1U`）时额外接受 TRIANGLE/QUAD 记录，
  旧 Host 对未知记录类型整批拒绝，Guest 须先检查该位；
- `RASTER_TEXTURE_UPLOAD {texture_slot, width, height, layout, pixels, length}` 把 INDEX8 纹理复制进 Host 资源池
  （无固定资源池配额；关闭 `CONFIG_MICROPIXEL_RASTER_KERNELS` 时不提供 CAP_RASTER）。
  宽高为 1–65535；槽编号 0–255 可复用，不预分配全部槽，
  `layout` 为 `COLUMN_MAJOR`（`pixels[u * height + v]`，供 COLUMN 记录）或 `ROW_MAJOR`（供 SPAN_PAIR 记录），
  `length == width * height`。建议使用 2 的幂尺寸以启用移位/掩码快速采样。重复上传同一 texture_slot 替换旧纹理；被拒绝的上传（`INVALID_ARGUMENT`、
  `INVALID_MEMORY`、`RESOURCE_EXHAUSTED`）保留旧纹理；
- `RASTER_PALETTE_UPLOAD {palette_slot, light_levels 1..32, pixels, length}` 向调色板槽上传 `light_levels x 256` 个
  canonical RGB565 word，`[light_level][index]` 是纹素 `index` 在光照级 `light_level` 下写入的颜色；每条记录用
  `palette_slot` 选调色板；Host 按目标 surface 的 `native_flags` 自行换序，Guest 不关心面板字节序；
- `RASTER_WARP_UPLOAD {warp_slot, width, height, row0, row_count, entries, length}` 上传 screen→纹理映射表的
  `row_count` 行，每项 32 位：低 12 位 u、接着 12 位 v、bit 24..28 为 light，最高位 `WARP_ENTRY_SKIP` 表示不画，
  `WARP_ENTRY_SOLID` 表示低 8 位是直接查调色板的索引。空槽或尺寸变化时接受部分上传，其余行为 SKIP，
  以便分帧流式上传大表；尺寸相同则原地替换这些行；
- `MICROPIXEL_GRAPHICS_CHANNEL_RASTER` 提交一个 24 字节 `micropixel_raster_header_t`（magic `'MPRS'`、目标
  `surface_handle`、`buffer_index`、`record_count`）加连续记录，总长不超过 `graphics_info.max_raster_bytes`
  （没有 `CAP_RASTER` 时为 0）。目标是 `surface_handle` 指定的 Direct Surface 的 Host buffer
  `buffer_index`；Host 先取得真实宽高和 pitch，再验证记录，Guest 不提供目标几何。
  目标不存在或 handle 已失效返回 `NOT_FOUND`，buffer 在飞或所需调色板缺失返回 `STALE_STATE`。记录类型：
  `COLUMN {x, y0..y1, texture_slot, light_level, u, v_start, v_step}` 沿一列按 16.16 步进取 `texel(u, v >> 16)`，
  `TRANSPARENT_INDEX0` 跳过纹素 0（世界 sprite）；`SPAN_PAIR {y_floor, y_ceiling, x0..x1, floor_texture_slot,
  ceiling_texture_slot, light_level, s, t, ds, dt}` 用同一条 (s, t) 走线填地板行和镜像天花板行；
  `SPRITE {x, y, width, height, texture_slot, light_level, source_x, source_y, source_width, source_height, color}` 把 COLUMN_MAJOR 纹理的
  一块矩形最近邻缩放到目标矩形，目标可以部分出界由 Host 裁剪，`SOLID_COLOR` 让每个绘制的纹素写 `color`
  而不查调色板（字形图集、单色覆盖层）；`RECT {x, y, width, height, color, opacity}` 裁剪后填充，`opacity=255`
  直写，更小则按通道 blend，`opacity=0` 拒绝；`WARP {x, y, warp_slot, texture_slot, palette_slot, u_offset,
  v_offset, fill_color, u_fraction_bits}` 把整张映射表放到 `(x, y)`，每项取 ROW_MAJOR 纹理的
  `texel(((u + u_offset) >> u_fraction_bits) & mask, (v + v_offset) & mask)` 经该项 light 查调色板；
  `u_fraction_bits`（0..4）让表项 u 和 `u_offset` 的低位成为纹素小数，贴图可按亚纹素步进滚动，
  `纹理宽 << u_fraction_bits` 不得超过 4096；`FILL_SKIPPED` 让 SKIP 项写 `fill_color`；
  `TRIANGLE {flags, texture_slot, palette_slot, vertices[3]}` 与 `QUAD {…, vertices[4]}`（需要
  `CAP_RASTER_POLYGON`）填充凸多边形，顶点为 `{int16 x, y（12.4 定点目标像素）, uint16 u, v（8.8 定点纹素）,
  uint8 light}`，u/v/light 沿边和扫描线 affine 插值（无透视校正），取 ROW_MAJOR 2 的幂纹理的
  `texel(floor(u) & mask_x, floor(v) & mask_y)` 经插值 light 所在调色板行；顶点可任意绕向，QUAD 须为凸；
  `TRANSPARENT_INDEX0` 跳过纹素 0，`FLAT_COLOR` 忽略纹理、以 `vertices[0].u >> 8` 作调色板索引；像素中心
  决定覆盖，Host 按目标裁剪，零面积多边形不画。COLUMN/SPAN_PAIR/SPRITE/TRIANGLE/QUAD
  各带 `palette_slot`。Host 先整体校验（COLUMN/SPAN_PAIR 坐标在目标内、SPRITE/RECT 尺寸非 0、slot 已占用且
  layout 匹配、`light < light_levels`、WARP 纹理为 2 的幂且表中最大 light 在调色板范围内、多边形纹理为
  ROW_MAJOR 2 的幂且每个顶点 `light < light_levels`、reserved 为 0），
  任一记录非法则整批拒绝且不写任何像素；校验通过后同步执行，`service_submit` 返回时像素已在 Host buffer 中。

### 像素格式与文字

Graphics/Resource 的 `MICROPIXEL_PIXEL_FORMAT_RGB565` 值为 `3`，表示内存中的 canonical little-endian
RGB565 word：bit 15..11 为 R、10..5 为 G、4..0 为 B；紧凑行宽为 `width * 2`。它与 panel wire byte
order 无关，末端 transport 若需要高字节先发，必须在 panel 层单独换序。BGR888、BGRA8888 的既有值和
字节含义保持不变。动态纹理更新必须使用创建纹理时的原生格式；Host 对 stride、矩形、乘法
溢出和精确 payload 长度再次校验。透明纹理仍使用 BGRA8888，并可直接 blend 到 RGB565 destination。

文字命令传递稳定的 System Font role handle（Small、Medium、Large、Title），不传具体像素字号。
Guest 使用逻辑像素定位；Host 可以按设备密度、语言和字体可用性为这些角色选择实际字形与字号。

## Resource

- `TEXTURE_LOAD {asset_id, scale_numerator, scale_denominator}` 同步加载 Bundle 位图；比例作用于素材
  的作者尺寸，`1/1` 保持原样。响应 `micropixel_texture_info_t` 同时给出 Guest 寻址的逻辑尺寸
  `width/height` 与实际存储的 `physical_width/height`，两者只在缩放加载时不同。SDK 的
  `TextureScale::kNative` 发送 `1/1`，`kDisplay` 发送显示变换的比例。
- `FONT_LOAD {asset_id}` 与 `FONT_UNLOAD` 管理 Bundle 字体；字体句柄与其他资源句柄一样为 32 位。
- `DYNAMIC_TEXTURE_CREATE`/`DYNAMIC_TEXTURE_UPDATE` 创建/更新动态纹理；请求只携带已校验的 Guest 像素
  范围，返回新的不可变快照句柄，`flags` 携带 `TEXTURE_FLAG_DYNAMIC`。UPDATE 不修改或释放输入句柄。
  SDK 在成功后更新逻辑引用并释放自己的旧引用；上一张 Scene 仍独立持有旧快照。更新失败不改变原资源，
  也不触发画面发布；下一次 Present 才使用新快照。BGRA8888、BGR888、little-endian RGB565 支持带 pitch
  的矩形更新。Host 校验范围、格式、长度、Session 句柄和容量后准备完整候选副本，失败释放候选，不发布
  部分更新。
- `TEXTURE_UNLOAD` 撤销任一纹理（Bundle 或动态快照）的 Guest 引用。
- 压缩素材由 Host worker 解码；同步等待期间暂停 Guest watchdog，不产生 Resource-ready event。
  Bundle raw format `10` 为 little-endian opaque RGB565。

## Audio

`GET_INFO` 上报混音率、`max_voices`、波形集合、`max_tone_duration_ms`、`max_clips`、`max_playbacks`、
`max_pcm_streams` 与 capability 位。`TONE_PLAY` 播放有界合成音；`STOP_ALL` 停止本 Session 的全部 tone、
playback 与 PCM 流。

Ogg Opus source/playback 分层：

- `CLIP_LOAD/UNLOAD` 管理 Bundle asset 的压缩来源 handle；只接受 Bundle format `ogg_opus`；
- `PLAYBACK_START` 创建一次播放实例，随后可 `PAUSE`、`RESUME`、`SET_VOLUME`、`GET_STATE` 或
  `STOP`；最多同时两条 compressed playback，clip 与 playback 配额由 `GET_INFO` 返回；
- playback 独立 pin clip。Guest 释放 clip handle 不会中断已经开始的播放，播放终止后 Host 才撤销 pin；
- Host 混音率是板级参数并由 `GET_INFO.sample_rate` 如实上报（Claw4 与 S3 板 16 kHz、Mosaico 32 kHz），
  Opus 始终解码为 16 kHz mono 再按整数比线性插值上采样；Host 拥有设备主音量。每条 playback 只有
  0..1000 的相对音量和 loop flag；
- 自然结束或解码失败投递 `AUDIO_EVENT_PLAYBACK_FINISHED`，`source` 和 payload 都携带 playback handle，
  `status` 区分成功与失败；主动 `STOP` 不投递完成事件；
- 压缩数据直接读取当前 App Bundle 的只读映射，Guest 不上传 PCM 或 codec packets。网络 URL、下载进度、
  缓存与取消以后由 Resource/Network Service 管理，不改变 Audio source/playback 的所有权语义。

Guest 生成 PCM 的推流通道，能力位 `AUDIO_CAPABILITY_PCM_STREAM`：

- `PCM_STREAM_OPEN{sample_rate, channels, capacity_frames, low_water_frames, volume_per_mille}`：
  `sample_rate` 必须等于混音率或是其整数分频，Host 线性插值上采样；`channels` 为 1 或 2，立体声平均
  下混到单声道混音器；`capacity_frames` 决定 Host 在 PSRAM 中分配的固定环形缓冲（上限是 Host 策略，
  超出返回 `INVALID_ARGUMENT`；Host 可向上取整并在响应中返回实际容量）。每个 Session 同时只有一条流，配额由 `max_pcm_streams` 上报；
- `PCM_STREAM_WRITE`：header 后紧跟 `frame_count × channels` 个 int16，整个请求
  ≤ `MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES`（4096）。Host 只拷贝能放进环的前缀并返回
  `accepted_frames`/`free_frames`，Guest 以此获得背压；欠载时混音器播放静音而不结束流；
- `PCM_STREAM_CLOSE(handle)`；`STOP_ALL` 和 Session 结束也会关闭流。App 暂停不关闭流，缓冲数据在恢复后
  继续播放；
- 事件 `AUDIO_EVENT_PCM_STREAM_LOW_WATER{stream_handle, free_frames}`：缓冲帧数降到 `low_water_frames` 及以下时
  由音频任务非阻塞投递一次，下一次成功 WRITE 重新武装；`low_water_frames = 0` 关闭该事件。事件是
  advisory，队列满时不阻塞音频任务，而是保持武装等待下一个混音块重试。

## 设备发现与外设 Service

Devices 是设备目录，不替代具体能力 Service。`LIST(kind, first_index)` 分页返回不透明 `device_id`：
每页最多 `MICROPIXEL_DEVICES_LIST_PAGE_SIZE`（16）个，附带匹配总数 `total_count` 和 catalog
generation；Guest 逐页读取，页间 generation 变化则重新开始。`GET_INFO(device_id)` 返回 kind、parent、
capabilities 与显示名称。
应用必须保存并传递 `device_id`，不能把枚举 index、GPIO 号、I2C 地址或 Host 指针当作设备身份。
未来热插拔设备用 Devices added/removed event 和新 generation 通知；当前板载目录在一次 Session 中保持
不变，并且只登记初始化成功的 Peripheral。

- Sensors 按 `device_id` 查询类型和单位，再 `OPEN` 独立 handle。加速度、角速度与磁场使用不同的 typed reading，
  不是一个不断增加可选字段的万能 Sensor 对象。第一个 handle 启动 Host 最新值缓存，最后一个 handle
  `CLOSE` 后停止；应用按自己的节奏调用 `READ`，实际维数由 `SensorInfo::value_count` 和 sensor kind 决定。
  `SET_SAMPLE_INTERVAL` 配置缓存采样间隔，Host 不宣告 Sensor events；event 1 继续保留且不得复用。一个手柄以后可
  作为 parent device，其按键和子传感器仍分别走对应 Service。
- GPIO 把每根可开放物理引脚列为 `GPIO_LINE` device。应用枚举后可直接将任意一根以 input、output
  或 PWM 模式 `OPEN`；打开即取得 Session 内独占 lease，`CLOSE`/Session teardown 恢复安全输入状态。
  第一阶段没有出厂 binding、用途命名或权限声明流程。input 只有配置 rising/falling/both edge 时才订阅
  event；主动 `READ`、output 和 PWM 不需要 GPIO event worker。
- Haptics 使用 move-only handle 播放/停止有界时长的震动，自然结束投递 finished event；
  `GET_INFO` 的 `capabilities` 为 32 位。
- Power 用 power device id 读取电池百分比、充放电与外部供电快照。

所有 handle 都包含 generation 并由 Host 验证所属 Guest。Sensor、GPIO 与 Haptics 的运行时槽位是
Host 策略（当前分别为 8、16、2），超出返回 `RESOURCE_EXHAUSTED`；事件继续使用统一的固定容量 EventQueue。
Sensor 最新值留在 Platform backend 的固定槽位中；GPIO event 按 handle 合并，只保留最新状态。第一个 edge
handle 懒启动 worker，最后一个释放或 App Suspend 时停止；GPIO ISR 只写入最小 POD 队列，由任务上下文
转换为 Guest event。

## 事件

事件 envelope 固定为 48 bytes，包含 `service_id + event_id`、flags、source、Guest 单调时间、
sequence、status 和 16-byte payload。event ID 只在所属 Service 内解释；当前定义 Timer expired、
Input touch、Input semantic key、Audio playback finished、Audio PCM stream low water、Devices added/removed、GPIO edge、
Haptics finished 和 Core host wake。新增事件不会扩大 Core import 表。

- 周期 Timer 队列中同一 handle 最多保留一条记录。积压时 `elapsed_us` 累加，`missed_count` 统计未单独
  投递的 tick；队列满导致的 tick 也结转到下一次成功事件。
- Touch wire 坐标是 `int32_t`。`pressure_per_mille` 仅在 Input 宣告
  `MICROPIXEL_INPUT_CAP_PRESSURE` 时有意义，范围固定为 0..1000。GT911 不宣告该能力并始终写 0。
- `MICROPIXEL_INPUT_EVENT_KEY` 与 `MICROPIXEL_INPUT_CAP_KEY_EVENTS`：固定键码为方向、
  Confirm、Back、Menu 和按位置定义的 gamepad South/East/West/North，阶段为 Down、Up、Repeat、Cancel；
  Repeat 必须携带非零计数，其他阶段的计数必须为 0。ABI 不定义 A/B/X/Y 标签键码。
  Host 负责把设备标签与区域性的确认/返回习惯映射为稳定语义；事件不表示设备一定安装了物理键盘。

## 稳定性与安全规则

- 已发布的 Service、method、channel、event、field、capability 和 opcode ID 永不改义、永不复用。
  2.0 发布前的整理不受此约束；发布后新增只能追加。
- Core ABI major 不兼容时拒绝加载；Service major 必须相同，Host minor 必须不低于 Guest 要求。
- Host 在进入 Service Handler 前完成 Guest pointer/length 校验；Handler 仍验证 schema、上限和句柄。
- Guest 不持有 Host 指针。Timer、Texture 等资源使用 Guest-local generation handle，
  由 SDK 的 move-only RAII 对象释放。
- Retained scene 持有独立 Texture 引用。Guest unload 只撤销 Guest 引用；显示场景替换或 Session teardown
  后才撤销 scene 引用，两个引用都归零时才释放像素内存。
- Graphics scene wire 由 `micropixel_graphics_scene_header_t` 开始。Keyframe 必须完整声明 background、连续
  node slot、所有 Container 和每个 drawable 的 `NODE_LINK`，并以新的非零 generation、`base_revision=0`、
  `revision=1` 发布；Patch 必须精确
  引用 Host 当前的 generation 与 base revision，且 revision 只增加 1。Host 对本次提交期间容量固定的 scratch scene 完成
  所有 record、property mask、slot、Container、父链、同级顺序、坐标、UTF-8、Font 和 Texture 校验后才原子交换。任一字段失败不
  改变当前 scene；base revision 不匹配返回 `MICROPIXEL_STATUS_STALE_STATE`，SDK 下一帧发送新 keyframe。
  Node ID 在同一 scene generation 内稳定，0 是最底层；结构变化由 SDK 发送新 keyframe，并按创建顺序把
  当前存活节点重新压缩为连续 Node ID。Public SDK 的 `slot + generation` handle 不进入 wire，也不等同于
  Node ID。Container ID 0 表示 Scene root，父链不得成环；SDK 的 slot/generation 与 wire ID 分离，销毁
  container 时递归失效整个子树。未知 opcode、未知 property 位和非零 header flags 一律拒绝。
- 固定容量、配额、超时和背压属于 Host 策略；只有 wire-format 硬上限进入公共 ABI。
- Public Guest 链接必须使用 allowlist，禁止用全局 `--allow-undefined` 掩盖拼写错误或未授权依赖。

新增 import 必须同时给出无法用现有 Core/Service transport 表达的真机性能证据，并更新 C ABI、
Host 注册、Guest lowering、allowlist、内存安全负向测试和兼容性 fixture。
