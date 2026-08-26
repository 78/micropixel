# MicroPixel SDK Demo

这是一个统一编译、统一烧录并通过触摸界面导航的 SDK 功能演示应用。它不是自动化 conformance
test，也不为每项能力生成独立 AOT；协议、错误和边界验证继续放在 `guest/tests/`。

## 源码导航

| 功能页 | 主要 API | 源文件 |
| --- | --- | --- |
| Timer / Clock / Log | `Timers::Every()`、`TimerEvent::delta()`、`Clock::Now()`、`Log::Info()` | `pages/timer_demo.cpp` |
| Input / Random | `TouchEvent`、`InputInfo`、`Random::U32()` | `pages/input_demo.cpp` |
| Storage | `KVStore::GetU32()`、`KVStore::SetU32()` | `pages/storage_demo.cpp` |
| Resource / Atlas | 同步 `Resources::LoadTexture()`、`Frame::DrawTexture()` source rect、生成的 atlas metadata | `pages/resource_atlas_demo.cpp` |
| Audio | `Audio::info()`、`Audio::Play()`、`Audio::StopAll()` | `pages/audio_demo.cpp` |

`main.cpp` 只进入应用；`demo_app.cpp` 负责首页、返回按钮、显式页面表和唯一事件循环；
`demo_page.hpp` 只定义页面契约和少量共享 UI 工具。每个功能页直接调用 Public SDK，便于 AI 按文件名
定位真实用法。Renderer 没有独立页面：`demo_app.cpp` 统一创建和 present `Frame`，各功能页直接
向它写入自己的绘制命令。

首页、返回按钮和各页面操作按钮共用 `sdk/ui/button.hpp`：按下时用带 opacity 的 `FillRect()` 叠加反馈，
移出按钮会撤销按下态，只有移回并在按钮内松开才触发动作。这个应用也因此直接覆盖 Renderer
alpha blend 的实际交互路径，不需要为 Graphics 再单独做一页。

Resource 页使用 `assets/manifest.json` 描述一张 12 帧 PNG atlas。构建阶段只生成
`build/apps/demo/generated/demo_assets.hpp`，页面从这个头文件取得 `AssetId` 和帧布局。Demo 面向
SDK 初次使用者，界面文案直接使用英文字符串，避免给最小 App 引入不必要的 Localization 样板。
Blocks 和 Snake 展示完整的静态 Catalog 国际化方式。

## 构建与烧录

```sh
python3 tools/micropixel package guest/apps/demo
bash tools/p4.sh flash-apps /dev/cu.usbmodemPORT
```

兼容脚本 `bash tools/build_demo_bundle.sh` 只转发到同一条 manifest 驱动的命令。

`flash-apps` 会清空 App Store 并写入 Blocks、Snake 和 Demo 三个示例 App。

构建输出统一写入 `build/apps/demo/`。Audio 页显示 Host 返回的实际采样率；当前 P4 / Metalio-Claw4
Mode 1 音频硬件链路固定为 16 kHz。页面可以分别试听 sine、square、triangle 和 noise；
页面显示的是最近一次提交的 tone，不把异步播放状态误报成持续
`Playing`。离开页面会调用 `StopAll()`。Storage 页使用 `micropixel.demo` 的私有 KV namespace，
计数值会跨应用和设备重启保留。
