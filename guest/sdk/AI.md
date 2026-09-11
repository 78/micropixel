# AI 使用 MicroPixel SDK

本文描述 Windows 安装管理组件的 Preview 接口；仅下载独立 SDK 0.15.6 不会获得 setup、doctor 或版本锁管理。
先读取对应 Release 的安装元数据，不能猜测最新版本或使用仓库混合的 Latest Release。

## 安装

安装必须有用户安装 SDK 的明确意图。下载清单包含 `version`、`architecture`、`url`、`size_bytes` 和 `sha256`。
下面固定选择 0.16.1 Preview。自动发现使用专用 [SDK 索引](https://raw.githubusercontent.com/78/micropixel/sdk-channel/index.json)：
`stable` 为空表示尚无稳定 Windows SDK；仅在用户接受 Preview 时使用 `preview`。
`windows_installer` 是经过发布验证的安装器地址、大小和摘要。

```powershell
# This explicit version is the unsigned Preview. Do not infer it from Latest Release.
$metadataUrl = 'https://github.com/78/micropixel/releases/download/sdk-v0.16.1/windows-installer.json'
$meta = Invoke-RestMethod -Uri $metadataUrl
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
if (([Uri]$meta.url).Scheme -ne 'https') { throw 'HTTPS download required' }
if ($meta.architecture -ne 'windows-x64') { throw 'Unsupported architecture' }
$installer = Join-Path $env:TEMP 'micropixel-setup.exe'
Invoke-WebRequest -UseBasicParsing -Uri $meta.url -OutFile $installer
if ((Get-Item $installer).Length -ne $meta.size_bytes) { throw 'Size mismatch' }
if ((Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $meta.sha256) { throw 'SHA-256 mismatch' }
$log = Join-Path $env:TEMP 'micropixel-install.log'
$p = Start-Process -FilePath $installer -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /LOG=`"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Installer failed: $($p.ExitCode)" }
$mp = "$env:LOCALAPPDATA\MicroPixel\bin\micropixel.exe"
$setup = & $mp setup --version $meta.version --yes --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $setup.ok) { throw 'Environment preparation failed; see stderr' }
$doctor = & $mp doctor --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $doctor.ok -or -not $doctor.result.ready) { throw 'Environment is not ready' }
```

等待安装进程退出后才检查环境。不要假设当前终端 PATH 已刷新，不需要重启电脑。
不要禁用 SmartScreen、防病毒软件或证书验证；被系统拦截时向用户报告需要人工操作。
依赖下载失败时重新执行 setup，不能把安装器退出码当作编译环境就绪的证据。

## 日常流程

在项目目录执行：

```text
micropixel doctor --json
micropixel sdk status --check --json
micropixel build --aot-target riscv32-ilp32f --json
micropixel package --aot-target riscv32-ilp32f --json
micropixel publish --dry-run --json
```

示例目标 `riscv32-ilp32f` 用于 P4/S31；S3 使用 `xtensa`。`publish --dry-run` 自动验证两种架构。

新项目使用 `micropixel init <目录> --app-id <标识> --title <名称> --json`。
已有未锁定项目须先选择清单中的具体版本 `sdk use <版本> --yes --json`。
仅在用户要求运行到设备时执行 `run --no-follow --json`；持续日志跟随单独进行。
两台设备连接时显式指定已确认的端口，不猜测设备。

上传商店需要用户发布意图；`publish --dry-run` 只做预检。
安装 SDK、升级项目、运行设备和上传商店是不同操作，`--yes` 不是对其他操作的授权。
收到新版提醒不会自动升级。已获升级授权后使用 `sdk upgrade --yes --json`，
重新编译、打包并测试；源码迁移单独处理。回退使用 `sdk use <旧版本> --yes --offline --json`。

## 机器输出

有限命令的 stdout 是一个最终 JSON 对象；进度、编译器诊断和运行日志在 stderr。
不要把 stderr 合并进 JSON 输入。结构如下：

```json
{
  "schema_version": 1,
  "ok": true,
  "code": "ok",
  "result": {
    "sdk_version": "0.16.1",
    "toolchain_id": "windows-x64-<toolchain-digest>",
    "artifacts": [],
    "version_status": null
  },
  "error": null,
  "warnings": []
}
```

结果字段随命令变化。构建产物记录路径、类型、目标架构；版本状态包含当前版本、候选版本、
检查状态、时间、发布说明和下一步命令。提醒去重不影响 JSON。

| 退出码 | 含义 | AI 处理 |
|---|---|---|
| 0 | 操作成功（可能有新版提醒） | 检查结果及 warnings |
| 1 | 执行失败 | 根据 error 和 stderr 诊断 |
| 2 | 参数错误 | 修正命令，不重复盲试 |
| 3 | 缺少输入或显式选择 | 补充当前操作所需输入 |
| 4 | 环境或兼容性阻塞 | 准备依赖或报告不兼容，不绕过锁格式 |

`--offline` 明确禁止联网检查，并保留缓存时间；缓存缺失不是“已经最新”。
`run --json` 必须配合 `--no-follow`，无限日志流不属于最终 JSON 协议。

验收与脱敏报告见 [Windows 验收](https://github.com/78/micropixel/blob/main/docs/development/windows-acceptance.zh-CN.md)。
辅助脚本不上传数据，不操作设备；上传报告前人工检查备注中没有用户名路径、设备身份或凭据。
