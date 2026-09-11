# Windows SDK 验收结论（SDK 0.16.0）

验收日期：2026-09-11。本文保留指定版本的验证范围与限制，不代表后续版本已经通过同等验收。
复验方法见 [W01–W19 清单](windows-acceptance.zh-CN.md)，发布要求以
[SDK 发布政策](sdk-release.zh-CN.md#正式版发布政策) 为准。

Windows 11 与 S31/S3 的部分链路已验证；Windows 10、无缓存 GUI 首装、下载故障恢复、
双设备选择、物理断网及商店实际上传等仍需验收。安装器未签名。

## 环境与版本

| 项目 | 实际环境 |
|---|---|
| 操作系统 | Windows 11 Pro x64，10.0.26200，管理员账号 |
| 终端 | PowerShell 7.6.5；另用新 PowerShell 5.1 检查 PATH 与 doctor |
| SDK / 安装包 | SDK 0.16.0 Windows x64 unsigned Preview |
| 原管理组件 | 0.1.0-08db5596a806b70f；Python 3.13.12、pyserial 3.5 |
| 工具链 | windows-x64-14b0c01b909a3fab；WASI SDK 33、双目标 WAMRC、AOT v6 |
| A/B 验收版本 | 9000.0.1 / 9000.0.2，使用发布版测试索引 |
| S31 | ESP-Mosaico V1.0，固件 0.7.7，480×480 |
| S3 | LCKFB SZPI ESP32-S3，固件 0.7.7，320×240 |

此环境不满足 Windows 10 22H2 普通用户基线；结果仅适用于表中版本与环境。

已核验 Release 的 26 个文件大小、GitHub SHA-256 与发布校验清单。安装器摘要为
`b61bece269298ab0a19638ca2291a168534513f47276ef81e35ce490ff98819c`，Authenticode 状态为 `NotSigned`。
原始产物见 [SDK 0.16.0 Preview](https://github.com/78/micropixel/releases/tag/sdk-v0.16.0)。

## W01–W19 结果

部分执行的项目保持 `not_run`，备注列出已完成子项。W11 标记修复后的复验结果；原发布安装包的失败另行保留。

| 编号 | 结果 | 已验证行为及限制 |
|---|---|---|
| W01 | pass | GUI 协议接受、安装进度、完成页及 Finish 正常；显示环境就绪，实际 doctor ready=true。GUI 为已有安装及缓存上的重装，无缓存首装和失败重试未测。 |
| W02 | pass | 从系统和用户 PATH 启动新 PowerShell 5.1，找到启动器；doctor 为单个有效 JSON，两目标工具可用。未代替桌面手动新开终端验收。 |
| W03 | pass | 同一 EXE 静默卸载重装均退出 0，没有等待向导输入。 |
| W04 | pass | 现有终端通过绝对路径立即可用；准备前 doctor 正确报未就绪，准备后 ready=true。 |
| W05 | pass | 中文、空格、& 路径构建打包成功；重复 package 不改产物时间，修改头文件后 1 个对象重编译、20 个复用。 |
| W06 | pass | hello、Blocks 素材音效示例及验收 App 均完成 RISC-V/Xtensa 打包，锁文件保持不变。 |
| W07 | pass | S31 自动选择 riscv32-ilp32f，上传至 100% 并运行；ready 日志、截图及用户实际触摸/Function 键的 down/up 一致。 |
| W08 | pass | S3 自动选择 xtensa，安装运行成功；用户确认实际输入有效，截图 Touch/Key 均为 UP，日志匹配。该板正常运行时 BOOT 映射为 Confirm。 |
| W09 | pass | 两板截图有效；stop 后回大厅且状态 not_running；再次 run 复用相同 Bundle，不重复上传，App 恢复运行。 |
| W10 | not_run | S31/S3 串口占用分别约 0.26/0.28 秒明确报错，释放后恢复；S31 拔线约 0.27 秒报错，重插重试后同一 MAC 恢复且 App 未重启。首次重插未枚举原因未确定；未观察到两板同时在线，双设备误选保护未实测。 |
| W11 | pass | 修复后的隔离组件在 S3 上复验：Ctrl-C 仅停止日志，退出 0、无堆栈，App 继续运行且串口释放。原发布安装包在两板上曾报 KeyboardInterrupt 并退出 1；该旧包仍需通过新版本发布获得修复。 |
| W12 | pass | A 项目检查出 B，打包不改锁，构建成功。 |
| W13 | pass | 仅一个项目 A→B，双目标重新构建；另一项目仍为 A。 |
| W14 | pass | B→A 可离线回退并构建，app.json 不变。 |
| W15 | not_run | 未执行物理断网下载；中断下载清理/重试、失败保锁回归通过，不能代替实测。 |
| W16 | not_run | --offline 构建通过且状态明确为 offline；未物理断网。 |
| W17 | not_run | 双架构 publish --dry-run --offline 通过；未登录或上传商店。 |
| W18 | pass | 验收管理组件切换成功后恢复正式组件；卸载 PATH 从 1 条变为 0、重装回到 1；缓存与项目保留，缓存立即可用。未执行清空缓存选项。 |
| W19 | not_run | 未签名符合 Preview 身份；此环境 GUI 启动无安全拦截，不代表全新浏览器下载的 SmartScreen 行为已验收。无稳定签名。 |

## Ctrl-C 兼容性与回归入口

SDK 0.16.0 发布安装包在 Windows 日志跟随结束时会报告 `KeyboardInterrupt` 并退出 1。
隔离管理组件 `0.1.0-584c825f21e791d0` 的复验通过：Ctrl-C 停止日志跟随、退出 0，
应用继续运行且串口释放。该结果仅覆盖隔离组件，不等于已发布安装包的复验。

Windows 会向同一控制台的进程广播 Ctrl-C。
[进程控制模块](../../tools/manager/process_control.py) 在等待子命令期间使用临时 SIGINT
处理函数，由底层命令处理中断，并保留输出与退出码；结束或启动失败后恢复原处理器。
[Windows 回归测试](../../tools/tests/test_windows_toolchain.py) 覆盖中断广播、退出码传播与处理器恢复。

该环境另观察到默认 GBK 下的 UTF-8 子进程输出解码问题，以及 doctor 的 WASI 版本显示为
`unknown`。这些问题需在目标发布版本中单独确认。
