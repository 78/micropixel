# Windows SDK Preview 验收记录（2026-09-11）

本次完成 Windows 本机软件链路与 S31/S3 真机验收，并发现、修复了 Windows Ctrl-C 包装层中断问题。
完整 W01–W19 尚未全部通过，不能据此提升稳定版。本文是脱敏结论；安装日志、截图、设备身份与生成产物不入库。
验收标准见 [W01–W19 清单](windows-acceptance.zh-CN.md)。

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

实际系统和账号不满足 Windows 10 22H2 普通用户基线，下表通过项仅表示本次环境上的行为。
通过芯片 MAC 对应的 USB 序列号确认设备，标识仅用于本地内存比对，不记录在本文。

已核验 Release 的 26 个文件大小、GitHub SHA-256 与发布校验清单。安装器摘要为
`b61bece269298ab0a19638ca2291a168534513f47276ef81e35ce490ff98819c`，Authenticode 状态为 `NotSigned`。
原始产物见 [SDK 0.16.0 Preview](https://github.com/78/micropixel/releases/tag/sdk-v0.16.0)。

## W01–W19 结果

部分执行的项目保持 `not_run`，备注列出已完成子项。W11 标记修复后的复验结果；原发布安装包的失败另行保留。

| 编号 | 结果 | 已验证行为及限制 |
|---|---|---|
| W01 | pass | GUI 协议接受、安装进度、完成页及 Finish 正常；显示环境就绪，实际 doctor ready=true。本次 GUI 为已有安装及缓存上的重装，无缓存首装和失败重试未测。 |
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
| W19 | not_run | 未签名符合 Preview 身份；本次 GUI 启动无安全拦截，不代表全新浏览器下载的 SmartScreen 行为已验收。无稳定签名。 |

S31 原有 7 个、S3 原有 6 个 App 的 Catalog 摘要在验收前后完全一致；每板仅新增
`local.windows.acceptance`，最后检查 `app last-error` 均为空。未烧录固件、擦除商店或上传应用。

## Ctrl-C 修复与复验

Windows 将 Ctrl-C 广播给同一控制台内的进程。原生启动器已经等待子进程，但 Python 启动层的
`subprocess.call` 和管理层的 `subprocess.run` 同时收到中断，导致底层日志命令正常处理后，
外层仍打印中断错误或未捕获堆栈。

[process_control.py](../../tools/manager/process_control.py) 在 Windows 等待子命令期间临时设置
SIGINT 的空处理函数，让收到同一事件的底层命令负责处理，再保留其输出和退出码；结束或启动失败后恢复原处理器。
不使用会被子进程继承的 SIG_IGN，也不改变非 Windows 的中断行为。
两层包装均使用此入口，管理组件打包器将新模块纳入内容摘要和产物。

验证证据：

- [Windows 回归](../../tools/tests/test_windows_toolchain.py) 在独立隐藏控制台向两层包装和底层命令广播两次真实 Ctrl-C，验证两次均到达底层、退出码 0/3 原样传回、stderr 无堆栈；另检查启动失败后信号处理器恢复、非 Windows 中断仍抛出。
- 新组件由 `tools/windows/build_manager.py` 生成，build ID 为 `0.1.0-584c825f21e791d0`。在隔离缓存中使用原生启动器与新组件组装启动链，保留原生启动器要求的 bootstrap 路径，未替换用户安装或已发布产物。
- S3 上实际 `logs --source app -n 5 --follow` 后广播 Ctrl-C：原生进程退出码 **0**，只出现正常停止跟随提示，无 Traceback / Operation interrupted；App 保持 foreground，串口立即可读。
- 原 SDK/安装器 **0.16.0 发布文件仍有旧行为**。该复验不是新安装包发布验收；后续需发布包含修复的新产物并复验，不覆盖旧版本。

## 命令与剩余问题

本次相关回归命令（设置 `PYTHONUTF8=1`）：

```text
python -m unittest tools.tests.test_windows_toolchain tools.tests.test_sdk_manager tools.tests.test_sdk_release tools.tests.test_cli_json tools.tests.test_release_recovery -v
python tools/windows/build_manager.py --output <新的本地输出目录>
```

修复后 30 项测试通过，新管理组件组装成功。此前已完成 `verify_installed_sdk.py --public` 的公开下载、
清理开发工具 PATH、全新缓存、双目标构建、增量、素材、预检与 A/B 验证。该次运行移除了调用参数中的
`-ExecutionPolicy Bypass`，沿用本机 RemoteSigned，没有修改系统执行策略。

`bash tools/p4.sh test` 已尝试，但在 Guest 构建入口因未配置 WASI_CLANG/WASI_SDK_PATH 停止，
完整发布门禁未通过；本次不涉及 Host/Guest 源码行为修改。文档相对链接及 `git diff --check` 检查通过。

尚存独立问题：源码测试在默认 GBK 下有一项 UTF-8 子进程输出解码错误（UTF-8 模式通过）；
doctor 将 WASI 版本显示为 `unknown`，固定 manifest 缺少该展示字段。这两项不在本次 Ctrl-C 修复范围内。

稳定发布还需 Windows 10 普通用户验收、GUI 无缓存与故障恢复、双设备误选保护、物理断网、
商店实际上传的独立授权与验收，以及受信任签名和时间戳。
