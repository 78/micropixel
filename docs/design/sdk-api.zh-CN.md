# SDK 正式版 API 重构

本文是正式版迁移契约。迁移正在实施，**尚未达到 API 冻结条件**。
具体可用接口以 [SDK 头文件](../../guest/sdk/)和可执行 conformance 为准。
迁移在当前工作区基础上进行，不丢弃已有修改；源码和 Bundle 允许破坏兼容。

## 0. 图形方向：2.5D 走整数 Raster，不走通用浮点网格

正式版图形 API 分三层，三层都是正式接口，不存在"过渡期旧路径"：

| 层 | 面向 | 每像素执行位置 | 状态 |
|---|---|---|---|
| `Scene` | 普通 UI、2D 游戏 | Host LVGL 合成，局部 damage | 已迁移完成 |
| 2.5D 前端（`Raycaster`、后续 `Mode7Plane`、`SphereView`）+ `HostSurface` | maze、赛车、earth 等 2.5D/伪 3D | Host INDEX8 + 光照调色板整数内核 | Raycaster 已接入 maze，SphereView 已接入 earth |
| PS1 级多边形前端（`MeshRenderer`）+ `HostSurface` | 房间/传送门型 3D（古墓类探索、固定视角冒险、赛道） | Host `TRIANGLE / QUAD` affine 扫描线整数内核 | 已接入 tomb-explorer demo（§3.1） |
| `GuestSurface` | 内核无法表达的逐像素自绘 | Guest 自写像素 | 保留 |

不提供通用浮点 3D（透视校正贴图、逐像素深度缓冲、Lambert）。MCU 没有值得依赖的浮点吞吐，
把逐像素工作从整数内核换成浮点纹理寻址加 PSRAM 深度缓冲读写，会让全屏 3D 掉到个位数 FPS
（早期 `Scene3D` 实验从 35 掉到 3.8 FPS，根因正是逐像素透视除法、Z-buffer 与浮点光照）；
2.5D 与多边形游戏的通用层必须建在既有整数内核之上。多边形层因此是 PS1 式的：
affine 贴图、无 Z-buffer（排序表画家算法）、顶点光查调色板、大面细分掩盖弯曲。

因此 2.5D 的抽象放在 **Guest SDK**：几何（射线 DDA、投影、遮挡排序）每帧只有几百次运算，
在 AOT 里足够快；每像素填充继续由 Host 的 `Column / SpanPair / Sprite / Rect / Warp` 内核完成。
App 不再手写 DDA、深度数组、覆盖统计或球面采样循环。

### 0.1 三类游戏共用什么

"通用"不在几何层，而在两处：

1. **Host 内核层**：`Column`、`SpanPair`、`Sprite`、`Rect`、`Image`，以及为任意静态映射新增的
   `Warp`（Host 持有一张 screen→(u, v, light) 表，每帧只改 `u_offset / v_offset`）。
   每个内核都是"INDEX8 纹理 + 光照调色板 → 面板像素"的整数循环，不关心它在画墙、路面还是球面。
   调色板按 slot 上传，每条记录选自己的 slot，一个 App 可以同时有地表、植物、UI 各自的调色板。
2. **SDK 共享词汇**（`sdk/raster_world.hpp`）：`DistanceLighting` / `LightTable`（按距离查表的
   离散光照）、`Billboard`（站在地面的朝向相机精灵，索引 0 透明）以及"远到近排序 + 深度测试"的约定。

几何前端按题材各写一个，每个都很小（几百行、固定容量、只写 Guest 状态）：

| 前端 | 题材 | 每帧几何 | 产出记录 | 状态 |
|---|---|---|---|---|
| `Raycaster` | maze 类栅格世界 | 每列一次 DDA、门板、覆盖统计 | `SpanPair` + `Column` | 已接入 maze；同一 Host 上与迁移前手写渲染器连续 A/B，FPS 差异在 1% 以内，Host 记录数相同 |
| `Mode7Plane` | 赛车、卡丁车、俯视伪 3D 地面 | 每行一次透视投影（相机高度、俯仰、曲率偏移） | 单行 `Span`（SpanPair 的单行形式）+ 路边 `Billboard` | 待做，需要 Host 增加单行 `Span` 记录或允许 SpanPair 两行相同 |
| `SphereView` | earth 类球体 | 俯仰变化时按行重建 screen→(u,v,light) 表（双槽、分帧流式上传）；每帧只有 yaw 偏移 | `Warp` + 植物 `Sprite` + 背景 `Rect` | 已接入 earth；S31 原生 480、performance profile 约 33 FPS，迁移前 Guest 逐像素版约 16 FPS 且拖动时降到 240 |
| `MeshRenderer` | 房间/传送门型 3D、固定视角冒险、赛道 | 每帧几百到一两千个顶点的视图变换、近平面裁剪、背面剔除、排序表 | `Triangle` + `Quad`（affine、顶点光） | 已接入 tomb-explorer；S31 480×480 Direct Surface 上 `quads-1.5x` ≈ 39 fps / 83 ns/px（见 polygon-benchmark README） |

earth 迁移后 Guest 每帧只剩投影、拣选和排序（约 3 ms），Host `Warp` 内核约占帧时间 70%，
是这条路径的下一个优化点。物种点在素材生成时烙进地球贴图，不再是逐点 `Rect` 记录；
`Warp` 表每行记录非跳过区间，被大气圈完全覆盖的脏矩形不再做背景恢复。
`Warp` 内核受贴图 gather 的缓存命中率支配：同一球体贴图 1024x512（512 KB）时 146 ns/px，
1024x256（256 KB）时 97 ns/px，所以 earth 用 1024x256 加 16 行极地填充。
`Warp` 记录的 `u_fraction_bits` 让表项 u 与 `u_offset` 带纹素小数（earth 用 1/4 纹素），
低速自转不再是整纹素的走停抖动，植物按未量化的 yaw 投影即可与贴图同步。
俯仰重建：`SphereView::BuildRows` 只对左半盘做三角运算、右半镜像，`kCoarse` 偶行隔列采样、
奇行整行复制；earth 拖动时粗表约 60 ms 工作量（含 23 ms 上传），按每帧 14 ms 预算约 150 ms 落地，
松手后再补一张全精度表。Guest 全屏渲染 App 用 `--profile performance` 构建：`-Oz` 下同一重建慢一倍。
`DirectSurface` 的 App 帧不能靠固定周期定时器驱动：一帧超过一个周期时，帧间隔会被凑整到
周期的整数倍；应在 `kSurfaceReleased` 事件里开始下一帧。

不进入这一层的东西：逐像素透视校正、逐像素深度缓冲、逐像素光照。任意三角/四边形网格由
§3.1 的多边形层承担，但只在"可见性可控"的题材内（房间与传送门、固定视角、赛道）；
开放大场景、大量半透明粒子、依赖 Z-buffer 的穿插几何不在预算内（480×480 @ 20 fps 的像素/秒
约为 PS1 320×240 @ 30 fps 的 2 倍，overdraw 余量只有约 1.5×）。若未来需要 Z-buffer，候选是
分块深度（如 480×32 条带放 SRAM）+ 多边形 binning，在真机数据之后另行评估。

## 1. 对象与模块清单

| 模块 | 决定 | 迁移规则 |
|---|---|---|
| Application / Event | 保留 | 单线程事件循环、Stop/Resume、Service View 入口不变 |
| Clock / Duration / Random / Log | 保留 | 保持强类型时间、应用时钟和无热路径同步长日志 |
| Timer | 修改 | After/Every 返回 Result<Timer>，Cancel 返回 Result<void>；取消失败可重试，Reset 尽力清理 |
| Audio | 保留 | Clip、Playback、PCM 生命周期不合并；设备主音量仍归 Host |
| Input / UI | 修改图形绑定 | 输入仍是逻辑坐标；UI 修改待提交状态，不自行提交画面 |
| Storage / Localization / LaunchArguments | 保留 | 不新增无关功能；检查本地 accessor 与 Host 查询命名 |
| Devices / Sensors / GPIO / Haptics / PowerInfo | 保留 | 设备身份、lease 和 generation 不变；核对明确结束与 best-effort Reset |
| Result / Error | 保留 | 可恢复的资源、容量和提交失败返回 Result；无异常、RTTI |
| Resources | 修改 | 统一 Texture 身份，原始像素加载默认，显示适配显式选择 |
| Scene | 修改 | 直接修改 Guest 状态、Renderer 提交、可保留非活动场景 |
| SceneUpdate | 移除公开事务模型 | setter 不再携带 update；失败不回滚 Guest 状态 |
| StreamingTexture / TextureUpdateBatch | 移除 | ABI 2.0 只保留动态纹理快照更新，统一资源引用与格式校验 |
| DirectSurface（HostSurface / GuestSurface）/ RasterResources | 拆分为两种 surface | 统一帧背压在基类；Host 绘制与 Guest 写像素是不同类型，Raster 资源归 Session |
| Raycaster / raster_world | 新增 | SDK 内的 2.5D 几何前端与共享光照、billboard 词汇，输出 Raster 记录 |

Service View 可复制，Host Resource 使用 move-only RAII。创建或打开可能失败的资源返回
`Result<T>`。`Reset()` 和析构只 best-effort；要求可确认结果的 Close/Stop 返回 Result。
本地属性读取不冒充 Host 查询；setter 不触发 ABI 调用。

Scene 拥有节点，应用持有类型化非拥有 handle。Scene 销毁后所有旧 handle 失效；校验不得
先解引用已经释放的 SceneState。Host 对已经接受的帧独立持有资源引用，Guest 销毁对象不破坏旧画面。

## 2. Scene 与帧

公开入口：

```cpp
Result<Scene> Renderer::CreateScene(const SceneDescriptor& descriptor) const;
Result<void> Renderer::Present(const Scene& scene) const;
```

创建、销毁和属性修改立即改变 Guest 的待提交状态。Present 接受一个完整帧，不表示面板已经
扫描完成。失败时旧画面保留，Guest 的待提交状态也保留，允许修正或重试。删除的本地 handle
不会因提交失败重新有效。新一轮提交应包含此前未成功的变化。

允许保存多个非活动场景；切换时不能用另一场景的 patch 修订号更新当前场景。分批传输先准备
完整候选快照，全部验证成功后再发布，不能显示半帧。缓冲忙时返回可重试状态，并通过帧可用事件
唤醒 Guest，不忙循环。纯 2D 保留局部 damage、批次和滚动缓存。

## 3. Raycaster：SDK 内的 2.5D 前端

`sdk/raycast.hpp` 提供 `Raycaster`。App 提供地图格子数组、相机和 billboard 列表，Raycaster 负责
DDA、墙面与门板投影、地板/天花板行设置、覆盖裁剪、billboard 深度排序与逐列遮挡，输出
`RasterDrawList` 记录。每像素工作全部在 Host 内核里完成。

```cpp
struct RaycastCamera { float x, y, dir_x, dir_y, plane_x, plane_y; };
struct RaycastCell   { RaycastCellKind kind; uint8_t texture_slot; uint16_t open; };  // 4 bytes
struct RaycastGrid   { const RaycastCell* cells; uint16_t width, height; };            // row-major view
struct RaycastBillboard { float x, y, height, lift; uint8_t texture_slot;
                          uint16_t texture_width, texture_height; bool self_lit; };

Raycaster caster;
caster.Initialize(config);                 // 视口、纹理 shift、地板/天花板槽位、光照曲线
caster.Cast(camera, grid);                 // 几何，只写 Guest 状态
caster.DrawWorld(list);                    // SPAN_PAIR + COLUMN
caster.DrawBillboards(list, billboards);   // 深度排序 + 逐列 z-test 的 COLUMN
```

约束与语义：

- 地图是单位方格，墙高 1；`kWall` 为整格实心墙，`kSlab` 为向上滑入天花板的门板，`open` 为已抬起比例，
  `open >= 1` 视为可穿过。射线只记录最近一块门板，`open < 0.5` 时门板参与 billboard 遮挡。
  水平门、旋转门或任意几何不在此层。
- 墙纹理为 `kColumnMajor`、边长 `1 << texture_shift`；地板/天花板为 `kRowMajor`；billboard 为
  `kColumnMajor` 且以索引 0 透明。纹理和光照调色板通过 `RasterResources` 上传，Raycaster 不持有像素。
- 光照为按距离查表的离散等级，侧面墙额外减档；`self_lit` billboard 使用最高档。参数在 `RaycastLighting`
  中显式给出，不在 App 里维护第二份曲线。
- 视口宽度上限 `Raycaster::kMaxColumns`，billboard 数量上限 `kMaxBillboards`；所有状态为固定容量数组，
  `Initialize` 后不再分配。
- App 保留门的开关规则、敌人行为、武器、HUD、文本和自定义叠加；这些直接用 `RasterDrawList` 追加在
  `DrawBillboards` 之后。`Depth(column)`、`LightFor(distance)`、`Project(x, y)` 供 App 做自己的可见性与瞄准判断。

赛车等伪 3D 类型后续在同一层增加 `Mode7Plane`（透视地面 SPAN 生成）与路边 billboard 排序；
球体渲染（earth）是否进入该层取决于能否用现有内核表达，否则维持 GuestSurface。

### 3.1 MeshRenderer：SDK 内的 PS1 级多边形前端

`sdk/mesh_renderer.hpp` 提供 `MeshRenderer`。App 持有网格（顶点 + 带纹素坐标和角光的三角/四边形面）
与实例变换，MeshRenderer 每帧做视图变换、近平面裁剪、背面剔除、距离光照、大面细分、精确 scissor
裁剪与排序表；`Flush(list)` 从远到近输出 `Triangle / Quad` 记录，每像素由 Host `DrawPolygon`
内核完成（`texel = tex[(v>>8 & mask_y) << log2w | (u>>8 & mask_x)]; out = lit_row[texel]`，
行内整数步进，每多边形一次浮点梯度）。

```cpp
struct MeshVertex { float x, y, z; };
struct MeshFace   { uint16_t vertex[4]; uint16_t u[4], v[4]; uint8_t brightness[4];
                    uint8_t texture_slot, flags; };            // vertex[3] == kNoVertex 为三角形
struct Mesh       { std::span<const MeshVertex> vertices; std::span<const MeshFace> faces; };
struct MeshCamera { Vec3 position; float yaw, pitch, focal_length, near; };

MeshRendererPool<2048, 8> pool;         // App 静态内存：多边形槽 + 8 组 × 512 桶
MeshRenderer mesh;
mesh.Initialize(config, pool.storage(), pool.groups());
mesh.Begin(camera);                     // 清空排序表
mesh.Submit(room.mesh(), Transform3::Identity(), {.group = g, .scissor = portal_rect});
mesh.Submit(part, root * joint, {.group = g, .depth_bias = -0.3F});
mesh.Flush(list);                       // 组升序、桶远到近
```

约束与语义：

- 坐标系 x 右、y 上、z 前；面按从正面看逆时针列出，屏幕空间有向面积为正即背面（`kMeshFaceDoubleSided`
  例外）。纹理为 `kRowMajor` 2 的幂，单个面的纹素跨度必须小于 256（wire 为 8.8 定点，按 256 回绕）；
  `kMeshFaceFlatColor` 用 `u[0]` 作调色板索引；`kMeshFaceTransparent` 跳过索引 0。
- 无 Z-buffer：可见性由多边形平均深度分桶（`kBuckets = 512`，`far` 之外共用最后一桶）决定，
  `depth_bias` 让站在大地面上的角色排到地面之后。跨房间顺序由 `group` 表达：更远的房间用更小的组。
  穿插几何按多边形而非像素解析，题材需要自行避免。
- affine 贴图：近处大面（最远/最近深度比超过 `subdivide_depth_ratio` 且屏幕跨度超过
  `subdivide_min_pixels`）一分为四递归 `subdivide_levels` 层；裁剪过的面不再细分。
- 光照 = 角光 × 距离衰减（`full_distance` 内为 1，到 `dark_distance` 线性降到 0），量化为调色板行；
  `minimum` 保底。关卡把静态光烙进顶点，动态物体按所在房间的环境光整体调亮。
- 固定容量：`Storage` 由 App 提供，池满时丢弃多边形并计入 `Stats::dropped`；单个 `Mesh` 顶点数上限
  `kMaxMeshVertices = 1024`，`Initialize` 后不再分配，不依赖 libm。
- 房间/传送门可见性（从相机房间沿传送门递归，屏幕包围矩形作 scissor，输出房间顺序）放在 App 内
  （`guest/apps/tomb-explorer/world/room_world.*`），出现第二个同类游戏再抽入 SDK。

验收：`tools/tests/test_mesh_renderer.cpp`（变换、投影、剔除、近平面/scissor 裁剪、细分、排序、
Flush 记录顺序）与 `test_tomb_room_world.cpp`（关卡数据、传送门遍历可见集与顺序、扇区碰撞）在
Host 上运行；`guest/apps/polygon-benchmark` 在真机上采 ns/像素与 1×/1.5×/2× overdraw 帧率。
S31 480×480 Direct Surface（performance）实测：`fill` 60 ns/px / 40 fps，`quads-1.5x` 83 ns/px /
≈39 fps，`quads-2x` ≈33 fps，封闭 `room`（MeshRenderer）≈28 fps / ~2× overdraw；门槛
`quads-1.5x ≥ 20 fps` 已通过，第一版内容无需默认 `upscale=2`。详见
[polygon-benchmark README](../../guest/apps/polygon-benchmark/README.md) 与
[tomb-explorer README](../../guest/apps/tomb-explorer/README.md)。
`tomb-explorer --benchmark` 按脚本路线每 120 帧输出 render/present/wait、面数、多边形数与
估算 overdraw；同板实测全程 ≥20 fps（重负载段约 20–25，走廊约 23–30）。Host 微基准（开发机）
上 `DrawPolygon` 中等尺寸多边形约为 `DrawSpanPair` 的 1.5–2 倍每像素成本，小多边形更高。
若后续关卡 overdraw 明显高于 2×，再考虑 `--upscale=2`（S31 有 PPA）或收紧传送门 scissor。

## 4. Host 与协议

Guest 保存节点树、业务状态、层级变换与待提交变化；Runtime 负责 Public SDK 到 C wire 转换。
依赖方向维持 Runtime → Device contracts ← Platform，FirmwareApp 为唯一组合根。

不兼容 Graphics/Resource 使用新 Service 版本，继续使用七个 Core imports，不复用旧 ID。
Host 校验 pointer/length、乘法溢出、有限数值、索引、类型、generation、Session 与容量。
能力不足明确返回 Unsupported；旧 Bundle 在协商时明确拒绝。暂停、恢复、Stop、Trap 和
App 切换均覆盖所有资源与在飞帧的收尾。Raycaster 不引入新的 Host 校验面。

## 5. 迁移与验收

顺序：设计/基线 → 基础规则与 2D（已完成）→ Raycaster 与 maze 迁移（已完成）→ Host `Warp`
记录与 `SphereView`、earth 迁移（已完成）→ `Mode7Plane` → 回归/冻结。

主要设备为 S31/Mosaico。基线使用重构前工作区正式构建的 maze、earth 和本地 mario，固定场景、
输入、profile、分辨率、音频/HUD 条件。mario 不作为仓库必需依赖，也不提交其素材。

每个场景预热后运行三轮、每轮 60 秒；记录实际呈现 FPS、P95 帧间隔、内存、Guest/Host
分段耗时和画面。均值 FPS 不得下降超过 5%，P95 不得增加超过 10%；不得用降分辨率、
删对象或关闭 HUD 达标。门槛只约束实际迁移的 App；maze 迁到 Raycaster 后 Host 记录流应与
迁移前等价，差异只来自 Guest 几何阶段。原始日志、MAC 和一次性数据仅保存在忽略的本地输出目录。

最低验证遵循 [仓库入口](../../AGENTS.md)：Guest 构建及相关 conformance、正式 App Bundle、
Host test 统一脚本、格式检查、S31/P4/S3 BOX-3 构建。发布或推送前运行 p4 test。
真机覆盖画面、触摸、音频、系统手势、暂停恢复及 App 切换；设备操作遵循
[S31 烧录流程](../development/flashing.zh-CN.md#esp32-s31--esp-mosaico-预览版)。

阶段状态：2D 已迁移；Raycaster 已接入 maze，S31 上与迁移前渲染器连续 A/B 的 benchmark FPS
差异在 1% 以内、Host 记录流等价、画面一致，三轮正式采样待执行。API 在功能、内存安全
与真机回归完成前不冻结。
