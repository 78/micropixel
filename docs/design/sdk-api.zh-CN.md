# SDK API 设计

本文说明 Scene、整数 Raster 前端与共享资源的设计契约。接口用法见
[SDK 文档](../../guest/sdk/README.md)，wire 布局见 [ABI](../../guest/abi/README.md)。

## 1. 图形模型与资源所有权

| 模型 | 适用场景 | 职责分工 |
|---|---|---|
| `Scene` | UI、2D 游戏、局部更新 | Guest 保存对象状态，Host 合成并维护 damage |
| `Raycaster` / `Mode7Plane` / `SphereView` + `HostSurface` | 栅格世界、透视地面、球体 | Guest 计算几何，Host 执行整数 Raster 内核 |
| `MeshRenderer` + `HostSurface` | 房间与传送门、固定视角冒险、赛道 | Guest 投影、裁剪和排序，Host 执行 affine 多边形填充 |
| `GuestSurface` | Host 内核无法表达的逐像素自绘 | Guest 生成像素，Host 管理呈现与缓冲归还 |

逐像素填充使用 INDEX8 纹理和光照调色板，减少浮点纹理寻址及 PSRAM 访问成本。
多边形路径采用 affine 贴图、排序表画家算法、顶点光照和大面细分；不提供逐像素透视校正、
Z-buffer 或逐像素光照。开放大场景、密集半透明粒子和依赖深度缓冲的穿插几何不属于当前能力范围。

几何前端共享 Host 的 `Column`、`SpanPair`、`Span`、`Sprite`、`Rect`、`Image`、`Warp` 等内核，
以及 `raster_world.hpp` 中的距离光照和 billboard 约定。调色板按槽上传，每条记录选择自己的槽。
前端采用固定容量状态；App 负责玩法、HUD 和题材专用的可见性策略。

`SphereView` 用 screen→(u,v,light) 映射表表示球面：俯仰变化时分帧重建并上传，
自转时只更新纹素偏移。`u_fraction_bits` 支持亚纹素偏移；粗表用于交互响应，全精度表用于稳定画面。
纹理尺寸和布局影响 gather 的缓存命中率，应结合目标板型测量。

Service View 可复制，Host Resource 使用 move-only RAII。创建或打开可能失败的资源返回
`Result<T>`。`Reset()` 和析构只做 best-effort cleanup；需要确认结果的 Close/Stop 返回 Result。
本地属性读取与 Host 查询分开，Scene setter 不触发 ABI 调用。Timer 取消失败可重试；
Audio 的 Clip、Playback、PCM 保持独立生命周期，设备主音量归 Host。

Scene 拥有节点，应用持有类型化非拥有 handle。Scene 销毁后旧 handle 失效；校验不得先解引用
已经释放的 SceneState。Host 独立持有已接受帧的资源引用，Guest 销毁对象不破坏旧画面。
资源使用统一 Texture 身份，动态纹理通过快照更新；显示或 surface 尺寸适配由调用方显式选择。

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

赛车等伪 3D 类型在同一层使用 `Mode7Plane`（透视地面 SPAN 生成，见 3.2）；路边对象排序仍由 App 完成。
球体映射由 `SphereView` 生成 `Warp` 记录；其他逐像素算法可使用 GuestSurface。

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

### 3.2 Mode7Plane：透视地面前端

`sdk/mode7_plane.hpp` 提供 `Mode7Plane`。`Initialize(Mode7PlaneConfig)` 按视口、地平线、相机高度和焦距
为地平线以下每一行求出 `depth = camera_height * focal_length / (y + 0.5 - horizon)` 与 `scale`，
裁掉 `near_depth`/`far_depth` 之外的行，并按 `shade_near..shade_far` 分配光照等级。每帧 App 只写每行的
`centre`、`half_width`、`style`（纹理行）、`texture_slot`（mip 槽位）和 `visible`，可用 `PlaceRows`
或直接遍历 `rows()`；`Draw(list)` 为每个可见行产出一条单行 `Span` 记录（type 9，28 字节）：
s 以 16.16 从条带左边到右边走完 0..1，t 取纹理行中心，`RowExtent` 给出该行落在视口内的像素范围，
供 App 决定背景只画路面之外的部分。

约束与语义：

- 地面纹理为 `kRowMajor` INDEX8，每行一种路面样式（如沥青/虚线/终点），多级 mip 各占一个纹理槽，
  App 按行像素宽度选最接近的一级，避免远行跳纹素。纹理和光照调色板通过 `RasterResources` 上传。
- 行数上限 `kMaxRows`（768），状态为固定容量数组，`Initialize` 后不再分配。
- App 保留赛道曲率采样、赛车/路边对象投影排序、HUD 与文字；这些用 `Image`、`FillRect`、`Text`
  追加在 `Draw` 之后。

HostSurface 帧应结合缓冲释放事件与帧调度续帧，避免超时帧被周期定时器取整到更低帧率。
缓冲数量需同时满足渲染、呈现和内存预算。

## 4. Host 与协议

Guest 保存节点树、业务状态、层级变换与待提交变化；Runtime 负责 Public SDK 到 C wire 转换。
依赖方向维持 Runtime → Device contracts ← Platform，FirmwareApp 为唯一组合根。

不兼容 Graphics/Resource 使用新 Service 版本，继续使用七个 Core imports，不复用旧 ID。
Host 校验 pointer/length、乘法溢出、有限数值、索引、类型、generation、Session 与容量。
能力不足明确返回 Unsupported；旧 Bundle 在协商时明确拒绝。暂停、恢复、Stop、Trap 和
App 切换均覆盖所有资源与在飞帧的收尾。Raycaster 不引入新的 Host 校验面。

不透明、不缩放的 RGB565 `Image` 记录是 Host 侧唯一可交给硬件的记录：`ExecuteDrawList` 把连续
符合条件（不透明、源尺寸等于目标尺寸、裁剪后宽 ≥32 且面积 ≥4096 像素、纹理字节序与目标一致）的
Image 攒成一批（最多 32 块），遇到其他记录、不合条件的 Image、满批或列表结束时通过
`Graphics::CopyOpaqueBlocks` 同步提交给设备的 DMA2D 引擎（P4/S31；S3 返回 Unsupported），
执行顺序与记录顺序一致，失败则整批由 CPU 内核重画。为了让拷贝不换格式、不换字节序，`ResourceService`
在 Guest 建立 DirectSurface 后按 surface 的 RGB565 格式解码之后加载的不透明纹理（RGB888 面板上 2D UI
纹理默认仍是 BGR888），并按面板字节序保存；先前已加载的 RGB565 纹理在首次被 Image 记录引用时原地转换
一次，并以 Host 内部的 `bitmap_flags::kRgb565ByteSwapped` 标记（Guest 不可见；flash 映射和动态纹理保持
规范序，由 CPU 内核逐像素换序）。Guest 侧用 `TextureScale::kSurface` 把纹理解码到 surface buffer 的
分辨率（面板比例再除以整数 upscale），这样在 720 面板用 360×360 buffer 时全屏背景也保持 1:1；
Guest 不感知硬件路径，也不提供异步提交。
