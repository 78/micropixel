# SDK 与 Windows Preview 发布

SDK、Windows 管理组件与工具链分别保留不可变的版本产物。SDK 使用 `sdk-v<版本>`，
工具链使用 `toolchain-windows-x64-<摘要>`；更新检查读取 `sdk-channel` 分支的 `index.json`，
不使用混有固件和工具链的 GitHub Latest Release。

## 日常 SDK 发布

1. 修改 `tools/micropixel` 的精确版本，更新 API 文档、迁移说明和验收状态。
2. 运行 `bash tools/p4.sh test`，提交 PR。Windows 验证会构建安装器并测试实际安装后的 SDK。
3. PR 验证通过并合并后，给同一提交创建 `sdk-v<版本>` tag 并推送。
4. `sdk-release.yml` 构建确定性 SDK 归档、内置运行时、安装器、清单和隔离的验收副本。
5. 安装后完成双架构构建、打包、发布预检、中文路径、增量依赖、A/B 升级回退和组件切换后，创建 Draft Release。
6. 发布为未签名 Preview，再从公开下载地址逐一核对产物摘要，并用全新缓存重新完成实际构建。
7. 公开复验成功后，才更新专用索引的 `preview` 和管理组件入口。`stable` 保持不变。

发布 workflow 从不上传应用商店、不烧录固件，也不覆盖已经存在的 SDK Release。
失败时查看具体步骤：Draft 创建之前可以修复并重新验证；已经创建 Release 后不要覆盖同名产物，
修复版本需使用新版本号。公开复验失败的 Preview 不会进入自动发现索引。

网站稳定 SDK 入口只引用经验证的 GitHub 归档及固定摘要，不重新打包当前工作区。
在 Windows 稳定验收完成前，网站原有独立 SDK 入口与 Windows Preview 分开维护。

## 工具链更新

工具链源和编译选项固定在 `tools/windows/toolchain-sources.json`、`build_toolchain.py`。
只有更新这些输入时才重新构建 `wamrc`。WASI 和 MSVC app-local runtime 各自固定摘要。

1. 先运行 `Windows toolchain verification`，确认两种目标的原生编译和应用验证均成功。
2. 从主分支运行 `Publish verified Windows toolchain`，输入该成功 run ID。
3. producer 检查 run 所属仓库、workflow、提交与当前固定编译配方，核对编译器及 CRT 摘要后发布独立工具链 Release。
4. 将已发布 `toolchain.json` 的 HTTPS 地址和 SHA-256 写入 `tools/windows/release-channel.json`。
5. 新 SDK 发布复用此工具链，不因普通 SDK 代码或文档修改重编译 LLVM/WAMRC。

`wamrc --version` 本身不能证明 AOT 兼容性；还必须保留固定 WAMR commit、来源摘要和 AOT v6 契约。

## 稳定版门槛

当前自动流程只发布 Preview，不能将未签名安装器或未验收版本自动提升到 `stable`。
稳定发布仍需 [W01–W19 真机验收](windows-acceptance.zh-CN.md)、两个设备架构实际运行、
关键故障关闭，以及受信任的 Windows 代码签名和时间戳。

签名后重新核对最终安装器的 SHA-256、下载清单和安装回归，不能沿用签名前的摘要。
在 Windows 上检查最终文件：

```powershell
Get-AuthenticodeSignature .\micropixel-setup.exe | Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
Get-FileHash .\micropixel-setup.exe -Algorithm SHA256
```

Preview 预期为未签名，必须如实记录；稳定包要求 `Status = Valid`，且发布者与正式签名身份一致。
签名基础设施与人工验收结果尚未提供，因此稳定签名与入口提升不会在当前 Preview workflow 中执行。
性能发布说明必须有对应场景的真机证据；没有证据时 `performance` 保持空数组。
