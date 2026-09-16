# 系统字体性能测试

用于 480×480、720×720 方屏和 320×240 横屏的 Guest 测试 App，通过公开 `SystemFont` 接口使用 Small、Medium、Large、Title
四个系统字体；不加载私有字体或字形图集。App 本身不选择字体后端，CBIN/Tiny TTF 由受测 Host 配置决定。
系统 locale 为 `zh-CN` 时使用中文静态标签，其他 locale 使用英文标签；动态数值保持相同。
日志记录 `labels` 与 UTF-8 字节数。不同语言的字形数量和覆盖面积不同，不能直接用两者 FPS 归因字体后端差异。
这些样本覆盖预热后的绘制，不代表大字符集持续换字、缓存淘汰的性能。

## 运行

准备 WASI SDK 和匹配的 WAMRC 后构建 release Bundle：

```sh
python3 tools/micropixel package guest/apps/font-benchmark --profile release --aot-target riscv32-ilp32f
python3 tools/micropixel --transport usb app install build/apps/font-benchmark/font-benchmark.bundle.bin
python3 tools/micropixel --transport usb app start micropixel.font-benchmark
```

一次运行依次测试：

1. **Scene**：15 条静态标签、15 个动态数值，以及标题和说明。仅修改数值节点，由 Host 决定脏区重绘。
2. **HostSurface / Raster**：相同位置、字号和数值序列，每帧清屏并通过 `RasterDrawList::Text` 重画全部文字。

320×240 使用 12 行（每种字号 3 行），避免小屏文字重叠；480×480 和 720×720 保持 15 行；720×720 按比例扩展位置，使用该设备的系统字号。日志记录行数和物理尺寸，
不同布局的性能数据不能直接比较。SZPI 构建时使用 `--aot-target xtensa`。

两种模式各预热 60 帧、采集 600 帧，不人为限制提交速率。Raster 在缓冲区忙时等待释放事件。
Scene 结束后留出 4 秒供截图，Raster 结束后保留最终画面，重新启动 App 可重复测试。
系统暂停/恢复会污染样本；若日志 `interrupted=1`，应重跑该组。

## 指标

`FONTBENCH result` 每种模式只输出一次统计，测量窗口内不写 Guest 日志：

- `submit_fps_x100`：Guest 成功提交速率乘以 100；Scene 可能合并或替换待显示帧，不能当成屏幕 FPS。
- `prepare_us`：更新数值及 Guest 节点的平均耗时。
- `draw_us`：Scene 的同步 `Present`，或 Raster 的 `Update` 平均耗时；包含相应 Host 同步工作。
- `present_us`：Raster 的 `Present` 平均耗时；Scene 已包含在 `draw_us` 内，因此此项为 0。
- `wait_us`：事件处理及等待空闲缓冲区的平均耗时。
- `p50_us`、`p95_us`、`max_us`：完整循环耗时，包含等待；统计单位为微秒。

比较时保持 Host 源码、内存池、App Bundle、字体文件、字号、显示设置和 HUD 开关一致。
应同时采集 Host 的文字测量/绘制耗时、presenter 实际呈现 FPS 和截图；不能仅凭 Guest 提交速率或 CPU
占用判断字体性能。截图、安装和诊断请求放在采样窗口之外。至少重复三次，并明确缓存是预热还是冷启动。
仅预热 Tiny TTF 字模不消除字距计算、字形查找和像素混合开销。
测试附加的字符描述/字距缓存时，保持源字模、字距、字号和 App Bundle 不变，并记录额外 PSRAM 占用。
