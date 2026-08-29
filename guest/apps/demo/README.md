# MicroPixel SDK Demo

这是一个统一编译、统一烧录并通过触摸界面导航的 SDK 功能演示应用。它不是自动化 conformance
test，也不为每项能力生成独立 AOT；协议、错误和边界验证继续放在 `guest/tests/`。

## 源码导航

| 功能页 | 主要 API | 源文件 |
| --- | --- | --- |
| Timer / Clock / Log | `Timers::Every()`、`TimerEvent::delta()`、`Clock::Now()`、`Log::Info()` | `pages/timer_demo.cpp` |
| Input / Random | `TouchEvent`、`KeyEvent`、`InputInfo`、`Random::U32()` | `pages/input_demo.cpp` |
| Storage | `KVStore::GetU32()`、`KVStore::SetU32()` | `pages/storage_demo.cpp` |
| Resource / Atlas | 同步 `Resources::LoadTexture()`、`SpriteNode::SetSource()`、生成的 atlas metadata | `pages/resource_atlas_demo.cpp` |
| Audio | `Audio::info()`、`AudioClip`、`Playback`、`Audio::Play()`、`Audio::StopAll()` | `pages/audio_demo.cpp` |
| Devices / Hardware | `Devices::List()`、typed `Sensor<T>`、`GpioInput/Output`、`Haptic`、`PowerInfo` | `pages/device_demo.cpp` |

`main.cpp` 只进入应用；`demo_app.cpp` 负责首页、返回按钮、显式页面表和唯一事件循环；
`demo_page.hpp` 只定义页面契约和少量共享 UI 工具。每个功能页直接调用 Public SDK，便于 AI 按文件名
定位真实用法。Renderer 没有独立页面：`demo_app.cpp` 统一持有 Scene 并提交 `SceneUpdate`，各功能页直接
向它写入自己的绘制命令。

首页、返回按钮和各页面操作按钮共用 `sdk/ui/button.hpp`：按下时修改对应 Shape/Sprite 的 opacity 反馈，
移出按钮会撤销按下态，只有移回并在按钮内松开才触发动作。这个应用也因此直接覆盖 Renderer
alpha blend 的实际交互路径，不需要为 Graphics 再单独做一页。BACK 使用加大的绘制区域和比绘制边界额外
大 6 px 的独立命中区域；Devices 页只为 Haptics、GPIO 和 Power 显示中间操作按钮，其他设备让 PREV/NEXT
均分整行。所有按钮文字统一使用向上 5 个逻辑像素的光学校正，与 Blocks、Snake 的操作按钮一致。

界面直接使用 `RendererInfo` 返回的物理坐标。`sdk/ui/layout.hpp` 的 `ComputeFlexLayout()` 在启动时计算
页面分区和首页菜单，在页面进入时计算操作按钮；480×480 使用单列紧凑布局，720×720 使用两列展开布局。
布局结果同时用于绘制和 Button 命中，触摸不做坐标转换。Flex 只分配矩形，不缩放 Texture。

Resource 页在两种屏幕上使用同一套 2× 物理规格的 RGBA 爆炸素材；30 帧平均放入三张紧凑 Atlas，
每张纹理都不超过 Host 的 720×720 限制。页面保留固定动画中心，不绘制或放大测试 canvas；各帧按
`canvas_position` 以物理像素 1:1 叠加在页面背景上。draw opacity 224 与素材 alpha 相乘，用来验收
texture alpha blend，不触发软件纹理缩放。

Devices 页不写死板型设备：它先用 `Devices::List()` 获得固定容量快照，再用不透明 `DeviceId` 调用
对应能力对象。PREV/NEXT 可以遍历全部设备；加速度计和磁力计显示 typed event；普通 GPIO 会以下拉
输入方式租用引脚，通过边沿事件显示真实电平，READ 按钮可以立即刷新；只输出 GPIO（例如 ESP-Mosaico
橙色状态 LED）以逻辑 OFF 打开，TOGGLE 在 ON/OFF 间切换并在离页时恢复 OFF。Input 页显示 Host 映射的
Function/Confirm 按键 pressed/released 状态及事件计数。Haptics 会播放 300 ms 震动，PowerInfo 显示电池与
外部供电状态。
离开页面时所有已打开对象通过 move-only RAII 自动释放。

`tools/generate_demo_explosion_atlas.py` 以原始 12 帧 RGBA 素材为输入，在离线阶段使用预乘 Alpha
时间插值生成 30 帧，把爆炸对象相对上一版放大 2 倍，再裁边打包成三张、每张 10 帧的 PNG Atlas；
设备端不做插值或缩放。Resource 页以 60 FPS 串联播放三张 Atlas；同一个 manifest 还把
`demo_music.ogg` 声明为 `ogg_opus` asset，Audio 页用它验收 Host 内置的 Ogg demux 和 micro-opus 解码。
构建阶段只生成
`build/apps/demo/generated/demo_assets.hpp`，页面从这个头文件取得 `AssetId` 和帧布局。Demo 面向
SDK 初次使用者，界面文案直接使用英文字符串，避免给最小 App 引入不必要的 Localization 样板。
Blocks 和 Snake 展示完整的静态 Catalog 国际化方式。

重新生成爆炸 Atlas 及其 manifest 帧区域：

```sh
python3 tools/generate_demo_explosion_atlas.py --frames 30 --update-manifest
```

## 构建与烧录

```sh
python3 tools/micropixel package guest/apps/demo
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT
```

`flash-apps` 会清空 App Store 并写入 Blocks、Snake、Demo 和四个 Showcase，共七个示例 App。

构建输出统一写入 `build/apps/demo/`。Audio 页显示 Host 返回的实际采样率；当前 Metalio-Claw4 与
ESP-Mosaico 音频硬件链路均为 16 kHz。页面可以分别试听 sine、square、triangle、noise 和 Bundle Ogg Opus；
页面显示的是最近一次提交的 tone，不把异步播放状态误报成持续
`Playing`。离开页面会调用 `StopAll()`。Storage 页使用 `micropixel.demo` 的私有 KV namespace，
计数值会跨应用和设备重启保留。
