# 创建、运行并发布第一个 App

从 `micropixel init` 开始创建自己的项目，用 `run` 在设备屏幕显示 Hello World，完成开发后用 `publish` 发布到应用商店。

## 1. 准备 SDK 和设备

下载并解压 [MicroPixel SDK](https://micropixel.ai/docs/releases/sdk/latest.json) 中 `archiveUrl` 指向的归档，使用同一文件中的 `sha256` 校验下载。归档地址为网站域名加 `archiveUrl`；不要只下载这份 JSON。
已解压 SDK 的用户可直接继续。完整下载命令、WASI SDK 与固定版本 WAMRC 的安装见[开发环境](https://micropixel.ai/docs/environment/)。普通 App 开发不需要 ESP-IDF。

在解压后的 SDK 根目录执行（macOS / Linux shell）：

```sh
export PATH="$PWD:$PATH"
export WASI_SDK_PATH=/path/to/wasi-sdk
export WAMRC=/absolute/path/to/micropixel-wamrc
python3 -m pip install "pyserial>=3.5,<4"
micropixel --version
```

后两条工具链路径必须换成实际安装位置。WAMRC 必须来自 MicroPixel 固定 fork、生成 AOT v6；不能用版本号相同的上游 WAMRC 替代。新终端需重新设置环境变量。

连接一台已烧录 MicroPixel 固件的设备，关闭其他串口工具：

```sh
micropixel --transport usb device status
```

没有固件时先使用[在线烧录](https://micropixel.ai/flash/)。多设备选择、Windows COM 端口和连接排错见[USB 本地开发](https://micropixel.ai/docs/usb/)。

## 2. 用 init 创建项目

```sh
micropixel init my-app --app-id com.example.my-app --title "My App"
cd my-app
```

将 `com.example.my-app` 换成你自己的稳定 App ID；发布后不要改成另一个应用的 ID。
脚手架自动生成：

```text
my-app/
├── app.json          # App ID、名称、0.1.0 版本、源码列表和能力声明
└── src/main.cpp      # 绘制 Hello World，进入事件循环
```

无需手写初始清单。已有代码可用 `micropixel init .` 扫描源码；已有 `app.json` 时会拒绝覆盖。

## 3. 用 run 在屏幕显示 Hello World

在项目目录执行：

```sh
micropixel --transport usb run
```

CLI 会构建、打包、安装并启动 App，随后跟随日志。设备屏幕应显示 **Hello, World!**，而不是只有终端输出。
按 `Ctrl-C` 仅退出日志跟随，App 继续运行。修改 `src/main.cpp` 后再次执行同一命令即可更新；它不会监听文件自动运行。

只安装启动、不跟随日志：

```sh
micropixel --transport usb run --no-follow
```

`run` 默认使用远程连接；此处显式选择 USB，无需 GitHub 登录或设备配对码。
希望使用不带 USB 参数的 `micropixel run` 时，先按 [Control API](https://micropixel.ai/docs/control/) 配置远程设备连接。设备连接与开发者发布登录是两套独立授权。

## 4. 开发完成后准备商店资料

Hello World 用来验证工具链。正式发布你的 App 前，准备：

- 至少 **2 张不同实际运行画面**，展示主要界面和核心玩法/功能；封面不计入这两张截图。
- 一段简短介绍：说明 App 是什么、用户可以做什么。
- 一份详细介绍和玩法/使用说明：操作方式、目标、胜负或完成条件、必要的设备能力。

可在不同游玩状态下分别截图；两次截图之间先操作 App 切换画面：

```sh
mkdir -p store
micropixel --transport usb screenshot --output store/01-main.jpg
# 在设备上进入实际游玩或另一个功能界面后，再执行：
micropixel --transport usb screenshot --output store/02-playing.jpg
```

图片使用 PNG/JPEG，每张不超过 2 MB，商店最多接受 8 张。将介绍保存在 `store/description.md` 方便维护；CLI 目前不会自动读取或上传这个文件及截图。
完整资料模板和发布限制见[发布应用](PUBLISHING.md)。

## 5. 用 publish 发布

检查 `app.json` 的 `version` 和 `requirements` 与实际实现一致，然后在项目目录执行：

```sh
micropixel publish --dry-run
micropixel auth github
micropixel publish
```

`--dry-run` 只做本地正式构建与发布校验，不登录、不上传。GitHub 登录在浏览器授权 CLI 后完成。
`publish` 上传正式 Bundle，成功后打印应用地址 `url` 和编辑地址 `editUrl`。

打开 `editUrl`，登录发布所用的 GitHub 账号，填写简短介绍、详细介绍和玩法/使用说明，上传至少两张截图并保存，然后打开应用页检查结果。
**现有流程先发布 Bundle，再编辑商店资料；两张截图和介绍是发布指引要求，目前不是 CLI 或服务器的强制门槛。**

后续更新先提升 `app.json` 的版本，再执行 `publish`。同一版本不能换成不同的 Bundle。
只修改介绍或截图时在网页保存即可，不必重发 Bundle。

## 下一步

- [发布应用](PUBLISHING.md)：资料模板、能力声明、实测设备与版本更新。
- [游戏开发指南](https://micropixel.ai/docs/app-development/)：游戏规则、输入、画面、音效和存档。
- [SDK API](https://micropixel.ai/docs/sdk/) 与 [示例 App](https://micropixel.ai/docs/examples/)：按需要查接口和完整实现。
