# 发布应用到 MicroPixel 商店

先用 `micropixel init` 创建项目、用 `micropixel --transport usb run` 在真机验证，再用 `micropixel publish` 上传正式 Bundle。第一次开发请从[快速入门](QUICKSTART.md)开始。

## 发布资料清单

每个正式发布的 App 请提供以下资料，帮助用户判断是否适合自己的设备、理解如何使用：

| 资料 | 内容与限制 |
| --- | --- |
| 运行截图 | 至少 2 张不同的真实 App 画面，建议主界面和核心玩法/功能各一张；PNG/JPEG，每张最多 2 MB，最多 8 张 |
| 简短介绍 | 说明 App 的用途或游戏特色；网页字段最多 240 字符 |
| 详细介绍和玩法 | 说明操作、目标、规则/完成条件和必要外设；非游戏 App 写使用步骤；最多 20000 字符 |
| 封面 | 与截图分开；Bundle 的 `launch_asset` 可作为默认封面，不能代替两张运行截图 |
| 版本与能力 | `app.json` 的 `version`、`requirements` 必須与实际 App 一致 |

不要用两张相同画面、封面或宣传图代替运行截图。介绍和截图应对应当前版本。
这些是开发者发布资料要求；**当前 CLI/服务端没有强制检查两张截图或说明是否齐全**，也没有待资料完善后才上架的草稿流程。

## 1. 在设备上准备截图和说明

运行并实际操作你的 App，在不同状态分别执行：

```sh
mkdir -p store
micropixel --transport usb screenshot --output store/01-main.jpg
# 切换到核心玩法或另一个功能界面
micropixel --transport usb screenshot --output store/02-playing.jpg
```

截图命令读取当前屏幕，不会自动导航 App。确保拍到的是 App 本身而非系统大厅。
建议在项目中保存 `store/description.md` 和图片便于版本维护；它们不会自动进入 Bundle，也不会由 `publish` 自动上传。

“简短介绍”示例：

> 通过倾斜设备控制小球穿过迷宫，在限定时间内到达终点。

“详细介绍”建议包含：

```markdown
## 应用介绍
说明核心功能或游戏内容，以及适合什么场景。

## 玩法与操作
如何开始；触摸、按键或倾斜分别做什么；目标是什么；
如何计分、胜利或失败，以及如何重开。
非游戏应用请写完成主要任务的使用步骤。

## 设备与注意事项
需要哪些输入或传感器；支持哪些画面布局；是否保存进度。
```

商店当前只展示段落和标题等受限 Markdown，避免依赖复杂表格、内嵌 HTML 或高级排版。

## 2. 核对应用清单

`init` 创建的新项目默认版本 `0.1.0`，并为 Hello World 生成基本能力声明。
增加触摸、音频、传感器或布局限制后，应更新 `requirements`，不要照搬初始声明。

以下是清单中的一个片段，适用于确实支持三种布局、必须触摸、音频可降级的 App：

```json
"version": "0.1.0",
"requirements": {
  "schema_version": 1,
  "display": {
    "layouts": ["square", "portrait", "landscape"],
    "min_width": 320,
    "min_height": 320
  },
  "required": ["input.touch"],
  "optional": ["audio.output"],
  "any_of": [],
  "services": {}
}
```

显示尺寸使用 SDK 的逻辑坐标，不是物理屏幕像素。`required` 是必须具备的能力，`optional` 缺失时 App 应正常降级；`any_of` 的每组至少满足一种，例如 `[["input.touch", "input.keys"]]`。
Service 版本以 `(major << 16) | minor` 编码。声明应以实际实现和测试为准。

发布会先构建并校验 P4/S31 的 `riscv32-ilp32f` 和 ESP32-S3 的 `xtensa` 两份独立 Bundle，再逐份上传。需要准备两种架构的 WAMRC（`WAMRC` 指定 RISC-V 编译器，`XTENSA_WAMRC` 指定 Xtensa 编译器）；要求单线程、内存检查开启、AOT v6，每份 Bundle 最大 8 MiB。

## 3. 校验、登录、发布

```sh
micropixel publish --dry-run
micropixel auth github
micropixel publish
```

也可显式传入项目路径：`micropixel publish ./my-app`。
`--dry-run` 执行正式构建和 Bundle 校验，不访问发布账号或上传产物。

可选的版本说明和实测设备：

```sh
micropixel publish --notes-file CHANGELOG.txt --tested-device metalio-claw4
```

`--tested-device` 可重复填写 `metalio-claw4`、`esp-mosaico`、`esp-box-3`、`szpi-esp32s3`、`m5stack-cores3`，只声明实际测试过的设备。
`--notes-file` 最多 3000 UTF-8 字节，描述本次版本变化，不能代替应用介绍或玩法。

首次成功发布绑定 App ID 的所有者。同版本同架构、同 Bundle 可重试；同版本同架构不能替换成不同内容。已有单架构版本可补发另一架构。
发布命令本身不会给用户设备自动安装或运行 App。

## 4. 完善商店页面

打开发布结果里的 `editUrl`，使用同一个 GitHub 开发者账号登录：

1. 填写“简短介绍”和分类。
2. 在“详细介绍”中填写应用介绍与玩法/使用说明。
3. 上传至少两张运行截图。
4. 保存并打开 `url` 检查文字、图片顺序和设备信息。

Bundle 中 `launch_asset` 对应的 PNG/JPEG（最多 2 MB）会作为默认封面；已有封面和介绍会保留。
首次发布后 Bundle 已可见，请及时补齐资料。目前不支持通过 `publish --screenshot` 或 `--description-file` 上传这些内容，不要使用不存在的参数。

## 更新与账号

新版本提升 `app.json` 的 `version` 后重复校验和发布；版本必须为 `major.minor.patch`，不支持预发布后缀。
介绍和截图可单独在网页更新。用户在设备或 Console 手动确认安装/运行新版本，当前不进行自动安装调度。

GitHub 开发者登录与设备 `auth pair` 独立。使用 `micropixel auth status` 检查发布账号，`micropixel auth logout` 撤销发布凭据。
发布凭据保存在用户私有配置目录，也可通过 `MICROPIXEL_PUBLISH_TOKEN` 注入；不要写入 `app.json` 或提交到源码仓库。
