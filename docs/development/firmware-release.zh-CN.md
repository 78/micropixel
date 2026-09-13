# 固件与 SDK 发布

普通开发者从网站安装 SDK 或使用在线烧录页面；本文仅供维护发布时使用。

## 先确定本次发布范围

| 改动 | 需要发布 |
| --- | --- |
| 只有 Host / 板级实现 | 新固件；Guest 来源一致时复用现有 SDK |
| 只有 SDK / Guest CLI | 新 SDK 和当前实现要求的同版 Windows 安装包；不无故重发固件 |
| Host 与 SDK / ABI 都有改动 | 两者都发，说明配套版本与旧固件能力回退 |
| 只有发布文档 | 更新文档，不生成新版本附件 |

安装包与项目升级的区别见 [SDK 发布说明](sdk-release.zh-CN.md#安装包是否必须跟随-sdk-更新)。

## 日常发布

1. 更新固件与 SDK 版本、相关 API 文档；提交与发布相关的源码。
2. 运行一次相关自动回归与格式检查。`bash tools/p4.sh test` 是本地完整检查入口，不必对每块板重复执行。
3. 创建 `sdk-v<版本>` tag。SDK workflow 验证安装与双架构构建一次，发布后核对同一批文件的公开摘要。
4. 从对应提交运行 **Build release firmware in parallel**。Windows job 按架构各编译一次预装应用；五个 Ubuntu job 独立构建 Host，共享 Host 回归只在 P4 job 执行一次。
5. 汇总 job 核对源提交、版本、芯片、OTA 容量、远控配置摘要和文件摘要，生成五板 OTA 与完整镜像。
6. 下载验证后的产物，再发布 GitHub / 网站，不在部署机器重新编译：

```sh
python3 tools/ci/download_firmware.py --run <成功的 run ID> --output build/release-<版本>/firmware
```

共享应用或汇总阶段失败时，可用 `reuse_hosts_run` 指定原 run，复用成功的 Host 文件，避免重复编译；只有部分板型失败时，同时用 `rebuild_profiles` 填写需重建的 profile（逗号分隔），其余板型必须在原 run 成功。汇总前会验证 Guest/firmware 源码没有变化，原 Host commit 与本次 workflow commit 分别保留。

下载器拒绝失败 workflow、非本仓库来源、缺少板型、源提交不一致或摘要不匹配的产物。
共享 Guest 与各板 Host 的原始提交分别记录，复用前检查实际源码一致，不能只比较版本号。
完整镜像按最后一个有效分区内容对齐，允许省略未使用的 flash 尾部；检查容量上限和内嵌 OTA，不要求文件长度等于整颗 flash。

维护者可直接使用以下入口（先替换版本和 run ID）：

```sh
gh workflow run firmware-build.yml --ref main
# 仅重建失败板型，复用原 run 中其他成功的板型：
gh workflow run firmware-build.yml --ref main -f reuse_hosts_run=<原runID> -f rebuild_profiles=metalio-claw4
```

`--ref` 必须指向本次发布的源码；若使用 main，先确认其 Host / Guest 仍是待发布版本。
`reuse_hosts_run` 只接受对应板型 job 成功的原 run；不能把失败 job 的残留文件视为合格产物。

## GitHub 与网站交付

1. SDK 自动发布使用 `sdk-v<版本>`；固件单独使用 `firmware-v<版本>`。不覆盖旧 tag 或已发布附件。
2. 固件按板型各打一个 ZIP，内含完整镜像、OTA、来源/摘要清单及许可证。Release 顶部提供在线烧录和板型下载表，不堆放验收报告。
3. 网站 SDK 归档与安装包直接复制已验证的 GitHub 文件，固定清单 SHA-256；不能在网站重新打包 SDK。网站版本、下载链接和更新索引保持一致。
4. 备份网站与固件目录清单，把新文件上传到新的版本目录。先校验文件，再切换固件清单、页面和 SDK latest 入口；保留旧目录与旧资源供回退。
5. 发布后回读网站：核对 SDK/安装包版本与摘要、各板固件版本及 OTA/完整镜像下载摘要，并检查页面链接。静态文件与固件目录更新无需重启 API。
6. 记录 tag、源码提交、各板产物来源、验证结果、备份位置与未完成事项。私有部署路径和服务配置只写私有运维文档。

## 维护者选择不等待 CI 时

这不是默认的全绿 CI 路径。维护者明确选择先行发布时，可以组合**已有成功构建**，不把等待整条 workflow 完成当成唯一发版方式：

- 本地板型必须已有相同源码、版本与发布配置的成功构建和相关回归；CI 板型必须来自成功的对应 job。
- 用 `firmware_artifacts.py collect` 收集本地板型，保留真实来源标记；各板目录按 profile 命名，再用 `assemble` 配合成功的共享 Guest 产物汇总。
- 收集和汇总时设置 `GITHUB_SHA` 为可核对的源码提交；检查工作区 Host/SDK 输入与该提交一致，不能给未提交源码伪造来源。按 README 准备环境和发布配置，不把秘密写入清单。
- 同样保留源码一致性、目标芯片、版本、容量、远控配置和 SHA-256 检查。不要通过修改下载器的成功状态检查来混用失败 job。
- Release 如实说明哪些板来自本地、哪些来自 Actions，以及尚未完成的 CI / 真机检查。后续发现实质问题发修订版，不替换已经公开的二进制。

## 固定输入与缓存

ESP-IDF commit 与板型列表位于 `tools/ci/firmware-sources.json`；WAMR 和 IOT solution 仍使用 Git submodule 固定版本。
IDF 自带工具下载清单验证编译器摘要，工具与 ccache 按板型/IDF 缓存。每块板使用独立 runner，不共享 managed_components 或 sdkconfig。
SDK 来源须与本次 Guest 源码一致；仅修复 CI 或 Host 时可复用已发布 SDK，但不能夹带不同 Guest SDK/ABI。
远控四项配置使用同名 GitHub Secrets；构建逐项对照生成的 sdkconfig，artifact 仅记录配置摘要，不输出配置内容。
修改 `sdkconfig.defaults` 中的选择项后，已有 `sdkconfig.release` 会保留旧值；本地发布前备份并重新生成受影响板型的配置，再核对 PSRAM、内部保留池与缓存参数，不能仅凭默认配置文件判断已生效。

## 按需执行的检查

- 安装器逻辑、内置 Python/pyserial、管理组件或更新机制有实质改动时，在 SDK workflow 手动启用 `full_validation`，增加 A/B 升级、回退与管理组件切换检查。
- 无发布前安装证据的恢复任务仍执行公开下载后的真实安装编译，不能仅检查文件存在。
- 真机检查针对本次行为变化，例如混合模式与 DirectSurface 截图；不要求每次重新填整份 Windows W01–W19。
- 未签名和尚未完成的 Windows 10 验收继续如实披露；按开源发布政策不阻塞正式版。

不要把维护者脚本、矩阵细节或验收清单加入普通用户安装指南。正式 Release 顶部按系统/板型提供直接下载或烧录入口。
