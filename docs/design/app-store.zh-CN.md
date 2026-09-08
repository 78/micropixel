# 应用商店发布与更新 v1

商店由 micropixel-control 的单实例 Fastify、SQLite 和持久产物目录提供。Guest 只声明需求，
Host 管理安装、生命周期及更新仲裁。设备授权与 GitHub 开发者身份独立。

## 清单与发布

`micropixel init my-app` 创建初始版本 `0.1.0` 的工程。已有清单仍可本地构建；公开发布必须具有
`version` 与完整 `requirements`。不支持预发布后缀或前导零，版本上限 31 个 ASCII 字符。

```json
{
  "version": "0.1.0",
  "requirements": {
    "schema_version": 1,
    "display": { "layouts": ["square", "portrait", "landscape"], "min_width": 320, "min_height": 320 },
    "required": [],
    "optional": ["audio.output"],
    "any_of": [["input.touch", "input.keys"]],
    "services": { "input": 65536 }
  }
}
```

以上是合并到既有 app.json 的字段，不是完整工程清单。Service 版本使用 `(major << 16) | minor`，
例如 `65536` 是 `1.0`；要求相同 major 且设备 minor 不低于要求。Core ABI 由构建工具写入 Bundle。
屏幕需求使用 Guest 的逻辑坐标，不是物理像素。Host 按 Guest Runtime 相同的短边 720 规则从显示服务转换：480×480 对应 720×720，320×240 对应 960×720；不能直接拿输入设备的像素尺寸判断兼容性。物理分辨率仅作为显示效果参考，不能据此拒绝支持缩放的应用。

布局来自当前逻辑屏幕宽高；边长范围 1–4096，最多 8 个替代能力组，每组至少满足一项。
支持 input.touch、input.keys、audio.output、sensor.acceleration、sensor.gyroscope、
sensor.magnetometer、haptics、gpio。可选能力不阻止安装，Guest 负责降级。

```sh
micropixel auth github
micropixel auth status
micropixel publish ./my-app --dry-run
micropixel publish ./my-app --tested-device metalio-claw4 --tested-device esp-mosaico --notes-file CHANGELOG.md
micropixel auth logout
```

发布固定 release 配置、开启内存边界检查、riscv32-ilp32f。实测设备是开发者声明，与能力匹配分开。
商店从已校验 Bundle 的 launch_asset 提取 PNG/JPEG 默认封面（最多 2 MB），保留已有封面与介绍；同一产物重试可补齐旧记录的封面。
CLI 将开发者凭据放在 XDG_CONFIG_HOME/micropixel/publisher.json（默认 ~/.config，0600），
有效期 30 天；CI 使用 MICROPIXEL_PUBLISH_TOKEN。MICROPIXEL_STORE_URL 默认为 https://micropixel.ai。
授权页面确认后一次性换取平台 Token，CLI 不持有 GitHub Token。`auth pair` 保持原语义。

## 设备契约

app.list 每个应用增加 `version`，响应增加 `freeBytes`。USB APP_LIST 条目在 SHA-256 后增加第七列
version；CLI 兼容旧的 5/6 列。Bundle 元数据增加 core_abi 与 requirements，旧清单保持可读。

device.snapshot.store 包含 protocol、target、coreAbi、width、height、capabilities、services、
idleMs、busy。配置可信商店公钥且为 P4/S31 时 protocol=2，否则为 0。协议 1 仅支持没有适配差异的安装；协议 2 允许手动安装带适配提示的版本。协议 0 不支持商店。

商店安装复用 app.install，新增 storeRelease（ES256 compact JWS）、automatic、baselineSha256。
JWS protected header 为 alg=ES256、typ=MPX-RELEASE、kid；payload 精确绑定 releaseId、publisherId、
appId、version、target、sizeBytes、sha256。签名采用 SHA-256 和 P-256，传输签名是 64 字节 R||S。
设备只从自身 Device Gateway 下 store/releases 路径获取授权产物，再验证签名、摘要、Core/Service ABI 及容量。
Kconfig 配置当前与上一把可信公钥的 DER SubjectPublicKeyInfo Base64 和 kid，以支持轮换。
P4/S31 发布 defaults 内置 micropixel.ai 的 store-v1 公钥；自建服务须替换为自己的公钥，私钥不得进入固件或仓库。
签名证明平台发布身份与完整性，不证明 AOT 代码安全或应用已经真机测试。

## 安装与更新不变量

安装使用 BundleFS begin_replace/write/open_staged/commit；在新包校验及 Catalog 提交前不删除旧包。
替换需要能容纳新旧两份数据和事务余量，空间不足时保留旧版本。失败中止 writer，应用存档不清理。
更新后的 Catalog 保持原应用顺序。详见 [BundleFS](bundlefs.zh-CN.md)。

服务端持久操作与设备、身份世代、应用、发布者、安装摘要关联；同设备串行安装，同应用合并任务。
丢失 Job 或服务重启后先查询 app.list，再决定是否已成功；不能凭丢失的 Job 重新安装。
更新检查只接管摘要匹配的商店安装收据，侧载或卸载使旧来源失效。
设备沿用固件检查时机查询应用更新，打开应用管理也触发检查；发现结果仅缓存在 RAM。
检查不创建安装任务。用户选择新版本后才创建持久操作；手动安装会停止运行中的应用，
不等待大厅空闲 30 秒。页面关闭不撤销已经确认的安装。

## 验证与上线

主仓库测试入口：Python CLI/Bundle tests、tools/tests/test_firmware_host.sh、格式检查与 P4/S31 Host 构建。
跨仓库能力词表和版本样例是 tools/tests/fixtures/app-requirements-v1.json，Control 的同名 fixture 必须一致。
Control 负责 API/网站/Console 的 typecheck、test、build 和 Caddy 校验。

生产先配置稳定认证密钥、GitHub OAuth、平台签名、官方应用归属和备份；随后升级支持签名的 Host，
开放手动安装与新版本检查；自动安装保持禁用，不提供自动更新开关。

上线门槛仍包括 P4/S31 真机安装/运行、适配提示后继续安装、ABI 拒绝、忙碌状态、断网、各写入阶段掉电恢复、存档保留，
以及使用正式域名的 GitHub 浏览器/CLI 授权。软件测试不能代替这些验收。
首版不包含评论评分、多实例、Xtensa 商店发布或业务崩溃后的自动回滚，不自动上架仓库本地应用。

## 手动安装的适配提示

Store 协议 2 将屏幕尺寸、布局和外设能力差异作为 Console 提示，用户仍可确认安装。Host 的 USB、商店安装及启动路径只强制验证 Bundle、签名（商店来源）、摘要、AOT 目标、Core/Service ABI、容量和生命周期约束，不按适配声明拒绝安装或启动。发现更新不会自动安装。协议 1 固件遇到此类差异仍需先升级 Host，服务端不假装已经解除设备侧限制。

## 当前更新交互：检查后由用户选择

应用新版本与固件检查一同定期查询，打开 App Management 也会查询。设备仅在 RAM 缓存版本与已安装 Bundle 摘要，不写入 Flash；重启后重新检查。只检查具有匹配安装摘要的商店来源应用，侧载替换使缓存失效。检测不会创建安装任务，也不提供自动安装开关。大厅点击有更新的应用时，action sheet 提供“安装新版本并运行 / 运行当前版本 / 取消”；手动确认后才创建持久安装任务，停止当前 App、安装，再运行。网页安装也不要求选择自动更新。服务器自动安装调度保持禁用，旧自动更新配置不再启用此行为。
