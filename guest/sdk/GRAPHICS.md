# Mode7 地面与 Surface 纹理

适用于 SDK 0.15.6 / 固件 0.7.7。先查询 `RendererInfo::raster_supported()`，再创建
`HostSurface` 和 `RasterResources`；新记录需要更新后的固件，旧固件的 raster 能力标志本身不能证明支持 `Span`。

## Mode7Plane

包含 `sdk/mode7_plane.hpp`。`Mode7Plane` 把透视地面变成每屏幕行一条 `Span`；App 负责赛道曲率、
纹理行选择、路边对象排序与 HUD。纹理通过 `RasterResources` 上传为 `kRowMajor` INDEX8，
每个纹理行表示一种地面样式，并上传对应的光照调色板。多级 mip 各占一个 texture slot。

初始化时用 surface 的 `buffer_width()` / `buffer_height()` 定义 `Mode7PlaneConfig::viewport`，
不要用面板物理尺寸代替 buffer 尺寸。配置 `horizon`、正数 `camera_height` / `focal_length`、
`near_depth` / `far_depth`、纹理尺寸和槽位，以及 `light_levels` / `shade_near` / `shade_far`。
`Initialize(config)` 返回 false 时停止使用该 plane。视口原点不能为负，高度最多 768 行；
光照等级为 1–32，纹理尺寸非零，深度范围须满足 `0 <= near_depth < far_depth`。
调用方传入有限数值，并将视口限制在 surface buffer 内。

每帧流程如下（`plane` 已初始化，`list` 来自 `HostSurface::Update()` 回调）：

```cpp
// 直线地面；偏移、相机横坐标和半宽使用同一世界单位。
plane.PlaceRows([](float depth) { return 0.0F; }, camera_x, road_half_width);
for (uint32_t i = 0; i < plane.row_count(); ++i) {
    auto& row = plane.rows()[i];
    row.style = 0;  // 换成该深度对应的纹理行，必须小于 texture_height。
}
if (!plane.Draw(list)) {
    return false;  // 把追加记录失败交回 Update 调用方。
}
```

`PlaceRows()` 根据深度计算每行的 `centre` / `half_width` 并使行可见；弯道可由回调返回该深度的横向偏移。
也可直接设置 `rows()` 中的 `centre`、`half_width`、`style`、`texture_slot` 和 `visible`。
`style` 为 uint8_t，因此可选择的纹理行是 0–255；不同 mip 槽位应共享纹理高度。
`RowExtent(row, first, last)` 返回裁剪后的含端点像素范围，可用于只绘制地面两侧背景。

`Project(depth, lateral)` 返回 buffer 坐标与像素/世界单位的缩放比；`visible` 仅检查深度范围，
不代表已通过屏幕边界或遮挡测试。路边对象按远到近排序，适当追加 `Image` / `Sprite` 记录。
`Draw()` 不建立 Z-buffer。plane 使用固定容量存储，应作为 App 的持久成员，避免每帧重建。
仍需遵守 surface 的 `AcquireFree()`、`Present()` 与 Released 事件背压流程。

## 直接提交 Span

`RasterDrawList::Span(y, x0, x1, texture_slot, light_level, s, t, ds, dt)` 是 `SpanPair` 的单行形式。
`x0..x1` 包含两端，调用方须先裁剪到目标 buffer；纹理为 `kRowMajor` INDEX8，纹理/调色板槽须已上传。
`s` / `t` 和步进是有符号 16.16：整数部分表示重复 tile，小数部分映射到纹理宽高，
不是直接以纹素为单位。每行可使用不同的步进，适合透视地面；检查返回值并处理 draw-list 容量不足。

## TextureScale::kSurface 与 Image

`resources.LoadTexture(asset, TextureScale::kNative)` 保留素材尺寸，`kDisplay` 适配逻辑画布，
`kSurface` 按显示比例再除以当前 DirectSurface 的整数 upscale 解码。
先创建 surface，再以 `kSurface` 加载需要适配其 buffer 的纹理；没有存活的 surface 时返回 `InvalidArgument`。
它不会把任意素材强制拉伸成整个 buffer，也不会在 surface 重建后自动重载现有纹理。

让 `Image` 的源区域与目标区域尺寸相同，可使用不缩放拷贝。P4/S31 的 Host 将符合条件的连续
不透明 RGB565 Image 合批交给 DMA2D：字节序一致、裁剪后宽至少 32 像素且面积至少 4096 像素，
每批最多 32 块。其他记录保持提交顺序；硬件失败时整批由 CPU 重画，S3 使用 CPU 路径。
这是 Host 优化，不新增 Guest 硬件接口，也不保证特定帧率；Guest 不应自行交换 Texture 字节序。
