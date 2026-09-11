# 固件与 SDK 发布

普通开发者从网站安装 SDK 或使用在线烧录页面；本文仅供维护发布时使用。

## 日常发布

1. 更新固件与 SDK 版本、相关 API 文档；提交本次源码，保留未提交实验。
2. 运行一次相关自动回归与格式检查。`bash tools/p4.sh test` 是本地完整检查入口，不必对每块板重复执行。
3. 创建 `sdk-v<版本>` tag。SDK workflow 验证安装与双架构构建一次，发布后核对同一批文件的公开摘要。
4. 从对应提交运行 **Build release firmware in parallel**。Windows job 按架构各编译一次预装应用；五个 Ubuntu job 独立构建 Host，共享 Host 回归只在 P4 job 执行一次。
5. 汇总 job 核对源提交、版本、芯片、OTA 容量、远控配置摘要和文件摘要，生成五板 OTA 与完整镜像。
6. 下载验证后的产物，再发布 GitHub / 网站，不在部署机器重新编译：

```sh
python3 tools/ci/download_firmware.py --run <成功的 run ID> --output build/release-<版本>/firmware
```

共享应用或汇总阶段失败时，可用 `reuse_hosts_run` 指定已完成五板 Host 的原 run，复用其文件，避免重复编译。汇总前会验证 Guest/firmware 源码没有变化，原 Host commit 与本次 workflow commit 分别保留。

下载器拒绝失败 workflow、非本仓库来源、缺少板型、源提交不一致或摘要不匹配的产物。网站目录保留旧版本，最后切换发布清单。

## 固定输入与缓存

ESP-IDF commit 与板型列表位于 `tools/ci/firmware-sources.json`；WAMR 和 IOT solution 仍使用 Git submodule 固定版本。
IDF 自带工具下载清单验证编译器摘要，工具与 ccache 按板型/IDF 缓存。每块板使用独立 runner，不共享 managed_components 或 sdkconfig。
SDK 来源须与本次 Guest 源码一致；仅修复 CI 或 Host 时可复用已发布 SDK，但不能夹带不同 Guest SDK/ABI。
远控四项配置使用同名 GitHub Secrets；构建逐项对照生成的 sdkconfig，artifact 仅记录配置摘要，不输出配置内容。

## 按需执行的检查

- SDK 安装器、管理组件或更新机制有变化时，在 SDK workflow 手动启用 `full_validation`，增加 A/B 升级、回退与管理组件切换检查。
- 无发布前安装证据的恢复任务仍执行公开下载后的真实安装编译，不能仅检查文件存在。
- 真机检查针对本次行为变化，例如本次加法混合与 DirectSurface 截图；不要求每次重新填整份 Windows W01–W19。
- 未签名和尚未完成的 Windows 10 验收继续如实披露；按开源发布政策不阻塞正式版。

不要把维护者脚本、矩阵细节或验收清单加入普通用户安装指南。正式 Release 顶部按系统/板型提供直接下载或烧录入口。
