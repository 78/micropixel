# Windows SDK FAQ 与版本管理

目标系统为 Windows 10 22H2 x64。SDK 0.17.0 提供未签名正式版安装器，发布 workflow 在双架构编译与安装验证通过后发布；
SDK 0.15.6 的独立归档不包含本文的安装管理组件。macOS 继续使用原有 SDK 流程。

从 [SDK 0.17.0 Release](https://github.com/78/micropixel/releases/tag/sdk-v0.17.0) 下载安装器和校验清单。
双击安装器，完成后检查环境就绪提示；下载中断可点击重试。

0.17.0 修复 Windows 日志跟随时 Ctrl-C 导致外层启动器打印中断堆栈的问题。由 0.16.0 升级时需要运行
新安装包，以更新安装器内置的 bootstrap；仅执行 `update` 或 `sdk use` 不能替换旧 bootstrap。
项目源码、既有锁文件和缓存保留，项目使用新 SDK 仍需显式选择版本。

## 安装契约

每个 SDK Release 提供同一个 GUI/静默安装包、`sdk-manifest.json`、安装包下载清单与 SHA-256。
安装器按当前用户安装到 `%LOCALAPPDATA%\MicroPixel`，不要求管理员权限；
内置 Python 3.13.12、pyserial 3.5 和管理组件，不要求用户安装 Git、CMake、LLVM、ESP-IDF 或 WSL。
安装器同时携带本版本 SDK 的校验清单；即使更新索引暂不可用，也可以按固定版本准备依赖。
依赖准备下载清单指定的 WASI SDK 33 和两种固定版本 `wamrc.exe`。

新终端中使用 `micropixel`；当前终端尚未刷新 PATH 时使用：

```powershell
$mp = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe"
& $mp setup --version 0.17.0 --yes --json
& $mp doctor --json
```

以最后一次 `doctor` 的 `ok: true`、`result.ready: true` 为环境可用标准。
安装器成功退出本身不代表依赖下载成功。断网后重新执行 setup；已完整验证的归档会复用。
未签名安装器被 Windows 拦截时需要用户核对发行来源，不能由 AI 关闭安全功能绕过。

## 固定项目版本

`init` 创建项目和 `micropixel.lock.json`。已有项目先显式选择版本：

```powershell
micropixel sdk use 0.17.0 --yes --json
micropixel sdk status --check --json
micropixel build --aot-target riscv32-ilp32f --json
micropixel package --aot-target xtensa --json
```

后续使用发布清单中实际存在的版本。将锁文件与源码一起提交到 Git。
锁记录精确 SDK、工具链 ID 和 SDK 清单 SHA-256。CLI、Runtime、ABI 随 SDK 一起固定。
依赖以内容摘要存放在用户缓存，多个项目可同时使用不同版本。

```powershell
micropixel sdk upgrade --yes --json
micropixel sdk use 0.17.0 --yes --offline --json
```

`sdk upgrade` 只选择稳定 SDK；也可显式 `sdk use 0.17.0 --yes`，已有项目不会静默升级。

升级先下载并验证全部依赖，再原子替换锁文件；失败保留旧锁。
回退时依赖已缓存可离线执行。两个操作都不改游戏源码、`app.json` 或应用版本号。
未知锁格式返回退出码 4，`--yes` 不会接受不兼容格式。
需要源码迁移时单独处理，并在升级后重新构建、打包和真机测试。

独立 SDK 归档仍允许手动设置编译器。受管理项目默认忽略机器上的编译器覆盖变量。
确需外部工具链时，先锁定版本，再显式启用：

```powershell
micropixel sdk use 0.17.0 --external-toolchain --yes --json
```

外部模式必须提供 `WASI_SDK_PATH`、`WAMRC`、`XTENSA_WAMRC`，结果标记 `external`，不能视为已验证的固定工具链。
再次 `sdk use <版本> --yes` 恢复受管理工具链。

## 新版检查

- `sdk status --check` 主动检查；`build`、`run` 使用一天内缓存。
- `package` 和 `publish` 使用五分钟内缓存，每次结果包含版本状态。
- `--offline` 不联网，返回 `offline` 和缓存时间；没有缓存时候选版本为空。
- 网络失败返回 `unavailable` 提醒，不阻止依赖完整的旧项目构建或发布。
- 只有比当前版本新的稳定 SDK 才提示升级。安装管理组件更新使用独立入口 `update --check` / `update --yes`。

日常人类提醒按项目和候选版本去重；JSON 始终包含当前检查状态。
性能提醒只有发布元数据同时提供描述、适用设备、所需固件和证据链接时才出现。
“最新版一定更快”不属于 SDK 承诺。

## 发布与验收

源码锁见 [toolchain-sources.json](../../tools/windows/toolchain-sources.json)，
内置运行时锁见 [runtime-sources.json](../../tools/windows/runtime-sources.json)。
[统一 SDK 打包器](../../tools/build_sdk_release.py) 为网站与 GitHub 生成同一确定性归档。
已发布的同版本文件禁止覆盖。

Windows 验收使用 [W01–W19 清单](windows-acceptance.zh-CN.md)。正式版要求自动验证通过。开源发布允许未签名安装器和未完成的人工验收，但须公开说明：当前已在 Windows 11 与 S31/S3 完成部分验收，Windows 10 和其余人工项目待完成。代码签名不作为发布前置条件；当前 CI 运行于 Windows Server，不能代替 Windows 10 验收。

AI 安装与使用契约见 [SDK AI 指南](../../guest/sdk/AI.md)。

## 卸载与缓存

在 Windows“应用和功能”卸载 MicroPixel SDK。界面询问是否清除已下载缓存，默认保留。
静默卸载同样默认保留缓存，不删除用户项目：

```powershell
$uninstaller = Get-ChildItem "$env:LOCALAPPDATA\MicroPixel" -Filter 'unins*.exe'
if (@($uninstaller).Count -ne 1) { throw 'Expected one MicroPixel uninstaller' }
Start-Process -FilePath $uninstaller.FullName -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait
```

仅在用户明确要求清理下载缓存时追加 `/PURGECACHE`。项目应保存在用户自己的工作目录，
不要放在安装器管理的 `versions`、`packages` 或 `downloads` 内。
