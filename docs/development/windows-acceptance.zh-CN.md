# Windows 10 SDK 真机验收

本清单针对 Windows 安装与版本管理项目。使用 SDK 0.16.0 Windows Preview 安装器；
setup/doctor/sdk/JSON 接口不能用独立 SDK 0.15.6 代替。
CI 通过不能替代本清单。没有执行的项目标记 `not_run`，不能填写通过。

已完成的本机及真机结果见 [2026-09-11 验收记录](windows-acceptance-2026-09-11.zh-CN.md)，
其中区分原发布安装包问题与 Ctrl-C 修复后的隔离组件复验；尚不满足稳定发布门槛。

## 准备

- Windows 10 22H2 x64，普通用户账号，最好用户名包含中文。
- 一款 P4/S31、一款 S3、两根可传输数据的 USB 线，设备已装匹配固件。
- Preview Release 中的安装包、摘要、对应版本的本文及辅助脚本。
- Release 提供独立验收版本 A=9000.0.1、B=9000.0.2 和 `test-sdk-index.json`。两者使用相同源代码，仅版本身份不同，用来验证升级机制，不代表性能差异。
- 测试商店发布时使用专用 App ID，不使用已有正式应用。

记录安装包版本、SDK A/B 版本、系统版本、设备板型和固件版本，不记录设备 MAC、凭据或配对码。
清理旧开发工具 PATH 后测试；无需卸载电脑上其他项目使用的 Python 或编译器。

## 安装和通用命令

先根据 Release 清单检查 `Get-FileHash .\micropixel-setup.exe -Algorithm SHA256`。
GUI 安装直接双击。静默安装使用同一包：

```powershell
$process = Start-Process -FilePath '.\micropixel-setup.exe' -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /LOG="install.log"' -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
$mp = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe"
& $mp setup --version 0.16.0 --yes --json
if ($LASTEXITCODE -ne 0) { throw 'Tool preparation failed' }
& $mp doctor --json
```

安装器成功不代表环境就绪。doctor 必须返回 `ok: true` 和 `result.ready: true`。
Preview 的系统安全提示需要人工核对，不关闭系统安全功能、不绕过拦截。

创建项目：

```powershell
& $mp init '.\中文 游戏\hello' --app-id local.windows.hello --title 'Windows Hello' --json
Set-Location '.\中文 游戏\hello'
& $mp build --aot-target riscv32-ilp32f --json
& $mp package --aot-target riscv32-ilp32f --json
& $mp package --aot-target xtensa --json
```

W07–W11 使用 Release 的 `windows-acceptance-project.zip`，解压后进入 `windows-acceptance` 目录，
在下载目录执行：

```powershell
Expand-Archive .\windows-acceptance-project.zip -DestinationPath .\硬件验收
Set-Location .\硬件验收\windows-acceptance
& $mp sdk use 0.16.0 --yes --json
```

该 App 在 240×240 及以上画面显示触摸与按键状态，
日志包含 `acceptance: ready`、`touch down/up`、`key down/up`；实际操作时画面和日志都应对应变化。
没有实体应用按键的设备记录“不适用”，仍需完成触摸测试。

连接设备后的命令：

```powershell
& $mp port list
# 仅替换为上一步确认的端口；两台设备时必须显式选择。
& $mp --transport usb --port COM7 run --no-follow --json
& $mp --transport usb --port COM7 screenshot --output '.\screen.jpg'
& $mp --transport usb --port COM7 app stop
& $mp --transport usb --port COM7 run
# 检查持续日志后 Ctrl-C：电脑端结束跟随，设备 App 继续运行。
```

## 结果表

每项填 `pass`、`fail` 或 `not_run`，失败补充最短复现步骤。不要把完整串口日志粘贴进报告。

| 编号 | 操作 | 预期结果 | 结果 |
|---|---|---|---|
| W01 | GUI 安装 | 无需另装开发工具，向导显示环境就绪 | not_run |
| W02 | 新开 PowerShell 执行 doctor --json | PATH 正确、单个有效 JSON、双架构工具可用 | not_run |
| W03 | 卸载后静默安装 | 无向导、无隐藏等待，退出码准确 | not_run |
| W04 | 不新开终端，用绝对路径运行启动器 | 不重启电脑即可使用 | not_run |
| W05 | 中文和空格目录内构建两次，再改一个头文件构建 | 第一次成功，第二次复用，改头文件正确重编译 | not_run |
| W06 | 分别打包 RISC-V/Xtensa | 目标、SDK、工具链和产物位置正确 | not_run |
| W07 | P4/S31 安装运行 | 画面、输入、日志正常 | not_run |
| W08 | S3 安装运行 | 架构识别正确、画面和输入正常 | not_run |
| W09 | 截图、停止、重新运行 | 图片有效，App 状态与命令一致 | not_run |
| W10 | 拔插、串口被占用、两台设备同时连接 | 明确报错、不挂死、不选错设备，释放后恢复 | not_run |
| W11 | 跟随日志时 Ctrl-C | 仅结束电脑端日志，游戏继续运行 | not_run |
| W12 | A 项目检查更新并打包 | 提醒 B，锁文件未变化，构建仍成功 | not_run |
| W13 | 两个 A 项目，仅升级其中一个到 B | 升级项目重编译，另一个仍使用 A | not_run |
| W14 | 将升级项目切回 A | 源码不变，可用缓存恢复 A | not_run |
| W15 | 下载中断网，然后重试 | 不假报成功、不破坏旧版本，可恢复 | not_run |
| W16 | 准备依赖后断网构建 | 构建通过，更新状态明确离线 | not_run |
| W17 | 专用 App 预检，单独授权后上传 | 双架构通过，商店版本正确 | not_run |
| W18 | 更新管理组件、卸载重装 | PATH 无重复、项目保留、缓存按选择处理 | not_run |
| W19 | 检查安全提示/签名 | Preview 提示如实记录；稳定包签名有效 | not_run |

更新实验在单独的新目录执行。以下脚本会创建两个测试项目，并显式执行 A→B→A，以及切换到附带的管理组件验收副本；
不上传商店、不操作设备。不要把测试索引用于正式项目。

```powershell
$previousIndex = $env:MICROPIXEL_SDK_INDEX_URL
try {
    $env:MICROPIXEL_SDK_INDEX_URL = 'https://github.com/78/micropixel/releases/download/sdk-v0.16.0/test-sdk-index.json'
    .\test_acceptance_versions.ps1 -Launcher $mp -Directory '.\Windows A B 验收'
} finally {
    $env:MICROPIXEL_SDK_INDEX_URL = $previousIndex
    & $mp update --yes --json
    & $mp setup --version 0.16.0 --yes --json
}
```

预期最后输出 `A/B update notice, explicit upgrade, dual-target rebuild, offline rollback and project isolation passed.`。
脚本检查打包没有改锁、另一项目仍用 A、`app.json` 未变、回退可离线构建。
W15 手动在 `setup` 下载期间断开网络，预期返回非零并保留旧项目锁；联网后重试。
W16 物理断网后在普通项目执行 `build --offline --json`，预期成功且版本状态为 `offline`。
W18 脚本另输出 `Manager update and bootstrap switch passed.`，表示当前进程结束后，新命令已使用新版本目录。
验收副本只有 build ID 不同，代码相同；finally 恢复正式索引对应组件。再手动完成卸载重装部分。

W17 复制验收 App 到独立目录，将 `app.json` 的 `app_id` 改为你拥有的新测试 ID，避免覆盖任何已有 App。
先执行 `publish --dry-run --json`；真实上传必须另行授权并登录，不由辅助脚本自动触发。

## 收集报告

运行 [collect_acceptance.ps1](../../tools/windows/collect_acceptance.ps1)：

```powershell
.\collect_acceptance.ps1 -MicroPixel $mp -Output '.\acceptance.json'
```

脚本只调用离线诊断，记录系统版本、受限制的版本字段、退出码和空的 W01–W19 结果表；
不安装、不升级、不操作设备、不上传数据。自行填写人工结果，分享前检查备注中没有个人路径或凭据。
若 PowerShell 策略阻止辅助脚本，记录提示并由用户按本机策略批准执行；AI 不自动修改执行策略。
安装日志只留本地；定位问题时另行提供经过脱敏的最小片段。

稳定发布须完成 W01–W19、解决关键故障、验证两个架构实际运行并提供签名安装包。
性能改进需要同一设备和场景对比 A/B，不以“更新到最新版”代替性能验收。
