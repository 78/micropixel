# SDK 与 Windows 安装器发布

SDK、Windows 管理组件与工具链分别保留不可变的版本产物。SDK 使用 `sdk-v<版本>`，
工具链使用 `toolchain-windows-x64-<摘要>`；更新检查读取 `sdk-channel` 分支的 `index.json`，
不使用混有固件和工具链的 GitHub Latest Release。

## 日常 SDK 发布

1. 修改 `tools/micropixel` 的精确版本，更新 API 文档、迁移说明和验收状态。
2. 运行 `bash tools/p4.sh test`，提交 PR。Windows 验证会构建安装器并测试实际安装后的 SDK。
3. PR 验证通过并合并后，给同一提交创建 `sdk-v<版本>` tag 并推送。
4. `sdk-release.yml` 构建确定性 SDK 归档、内置运行时、安装器、清单和隔离的验收副本。
5. 安装后完成双架构构建、打包、发布预检、中文路径和增量依赖检查，创建 Draft Release。安装器、管理组件或更新机制有变化时，手动启用 `full_validation`，追加 A/B 升级回退与组件切换。
6. 按 `tools/windows/release-policy.json` 指定的通道发布（当前为未签名正式版），再从公开下载地址核对核心产物摘要，关联本次全新安装缓存中的成功构建证据；同一批文件不重复安装编译。
7. 公开复验成功后，才更新对应 `stable` 或 `preview` 索引及管理组件入口。

发布 workflow 从不上传应用商店、不烧录固件，也不覆盖已经存在的 SDK Release。
失败时查看具体步骤：Draft 创建之前可以修复并重新验证；已经创建 Release 后不要覆盖同名产物，
产物本身有修复时需使用新版本号。公开复验失败的版本 不会进入自动发现索引。

仅公开下载或索引提升步骤失败时，可从主分支运行 `Reverify published SDK`，
输入已发布的精确 tag。它下载现有产物，核对 GitHub 摘要与发布校验清单，使用原安装包和
原验收项目完成 Windows 复验后再提升索引；不重新生成、替换安装包或 SDK。
公开下载对短暂的 404、限流与服务错误进行有界重试。重复提升同一版本不会重复提交索引，
较旧的 Preview 也不能覆盖已提升的较新版本。

网站稳定 SDK 入口只引用经验证的 GitHub 归档及固定摘要，不重新打包当前工作区。
网站 SDK 和安装器入口使用同一正式版本，并披露安装器签名和人工验收状态。

## 工具链更新

工具链源和编译选项固定在 `tools/windows/toolchain-sources.json`、`build_toolchain.py`。
只有更新这些输入时才重新构建 `wamrc`。WASI 和 MSVC app-local runtime 各自固定摘要。

1. 先运行 `Windows toolchain verification`，确认两种目标的原生编译和应用验证均成功。
2. 从主分支运行 `Publish verified Windows toolchain`，输入该成功 run ID。
3. producer 检查 run 所属仓库、workflow、提交与当前固定编译配方，核对编译器及 CRT 摘要后发布独立工具链 Release。
4. 将已发布 `toolchain.json` 的 HTTPS 地址和 SHA-256 写入 `tools/windows/release-channel.json`。
5. 新 SDK 发布复用此工具链，不因普通 SDK 代码或文档修改重编译 LLVM/WAMRC。

`wamrc --version` 本身不能证明 AOT 兼容性；还必须保留固定 WAMR commit、来源摘要和 AOT v6 契约。

## 正式版发布政策（2026-09-11 更新）

项目维护者明确授权放宽开源发布门槛。正式版要求自动化回归、安装生命周期、双架构构建、公开下载摘要和全新缓存验证通过。
代码签名和全部 W01–W19 人工验收不再阻塞正式发布；发布说明、安装元数据及网站必须如实披露未签名、Windows 10 待验收和其余未完成项。
已完成的 Windows 11 / S31 / S3 结果见 [验收记录](windows-acceptance-2026-09-11.zh-CN.md)，不能写成 Windows 10 已验收。

发布元数据的 `preview` 表示发布通道，`code_signing` 独立表示签名状态，正式版不意味着已经签名。
不得通过修改验收结果、覆盖旧产物或关闭 Windows 安全功能达成发布。
将来引入签名时应对最终签名文件重新生成 SHA-256，并完成安装回归；不能沿用签名前的摘要。
性能发布说明仍需对应场景的真机证据；没有证据时 `performance` 保持空数组。

## 面向用户的发布附件

正式 SDK Release 只提供安装包、SDK 归档和安装/更新必需的机器清单与管理组件，顶部按操作系统给出下载入口。
验收脚本、9000.x 测试 SDK、验收 App、报告与发布内部索引放在独立的 `sdk-support-v<版本>` 维护者 Release，绝不作为新手安装步骤。
常规发布验证正式下载文件的摘要；无本次安装证据的恢复 workflow 会合并下载两处原始文件，从独立验收索引执行实际安装与 A/B 检查。
普通安装文档只展示下载、安装和开始开发；诊断与校验脚本放 FAQ，自动化脚本放 AI 指南。
