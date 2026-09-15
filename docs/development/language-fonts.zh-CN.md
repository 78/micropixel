# 系统语言与 TTF 字体

系统正文默认使用 Tiny TTF。内置 Montserrat 提供英语和恢复界面；Noto 字体组件补充对应地区的字符。
四个系统字号共享同一份只读字体数据，Guest 系统字体句柄与 Host 使用同一字体服务。
首批 locale 为 `en`、`zh-CN`、`zh-TW`、`ja-JP`、`ko-KR`。

## 字体组件契约

字体采用 Bundle v1 的 `package_type=component`、`component_type=font`，没有 AOT，也不作为可启动 App。
通过 `micropixel publish` 发布到应用商店，使用现有版本号、不可变发布摘要与商店签名。
TTF 组件的元数据使用 `font: {"asset":"regular","format":"ttf"}`，保留 `languages`、
`font_bundle` 和 `charset`；其资源是单个 FONT section，format 11（静态 TrueType）。
已有四角色 `fonts`/CBIN format 8 的组件保持可读；两种声明不得混用。

系统字体组件安装到 NOR 系统 BundleFS，并持有只读映射；字模缓存、度量缓存和光栅化临时区使用 PSRAM。
映射必须覆盖所有使用该字体的字号的生命周期，活跃映射对应的物理块不得在更新中被重用。

## 查询、下载与切换

设备与 Control Server 的全部通信只使用已有 HTTP/3/QUIC 传输，不引入 HTTP/1.1/TCP 字体下载分支。
语言菜单使用本地支持列表，不为展示菜单等待联网查询。用户选中语言后，请求 Control API 获取该 locale
对应的可用字体组件版本、大小、摘要、签名和下载地址。下载路径由服务端返回，不固定在固件中。
当前语言单独置顶，仅显示正在使用状态，不可点击；其他语言列在独立分组。选择语言先打开确认面板，仅查询已签名的字体版本、下载大小和 NOR 可用空间；确认前不下载字体、不安装、不修改语言。空间不足禁用确认。确认后在同一模态面板显示下载、安装进度，阻止返回和切页；失败允许关闭，成功才保存并展示新的有效语言。
网络、空间、签名、字体或内存错误保留当前语言。可用的本地组件允许离线切换。

普通 App 和系统组件共用已安装包清单。`POST /device/v1/devices/{deviceId}/store/check` 的可选
`packages: [{appId, version, sha256}]` 一次提交所有已安装 Bundle（最多两个存储共 100 项），统一返回
`updates: [{appId, currentVersion, version, sha256, state}]`。`sha256` 是查询时的已安装摘要，用于拒绝过期结果。
缺省 `packages` 的旧设备沿用服务器缓存的 App 列表；显式空数组表示没有已安装包。历史 Bundle 的空版本表示未知，仍可上报但不提示更新。
清单从 BundleFS 目录读取，独立于 Hall 的可启动 App 筛选，不从当前语言临时拼接字体 ID。
没有字体专属查询字段或独立轮询；新增系统组件类型复用同一协议。内置英语不是安装包，因此不进入清单。
版本发现按包 ID 选择；语言到字体组件的查询只用于首次选择语言和获取确认安装所需的签名清单。
语言菜单有字体更新红点，当前语言下方仅在有更新时显示独立更新按钮；点击后复用字体查询接口和确认面板，
展示当前及目标版本、大小、空间和进度，确认前重新获取签名发布信息。同语言更新不写 locale 设置。
普通 App 更新红点属于 Manage Apps，列表在每个存储分组中将有更新的 App 前置；当前访问期间固定显示顺序，
动作仍使用原目录索引。字体在 Manage Apps 所属存储中以只读系统组件显示名称、版本和大小，不进入 Hall 或普通 App 自动更新队列。

切换先准备新字体，保留旧字体直到新字体激活成功，再释放旧映射并删除不再使用的系统字体组件。
下载后的签名字体先写入 NOR 暂存区，从验证通过的暂存映射准备所有字号，成功后才提交目录替换。
准备或提交失败时释放候选映射并中止写入，旧目录仍可恢复；同 ID 更新也遵循此顺序。
不得先删当前字体来腾空间；原子替换需要新旧版本同时存在的空间，不足时明确提示系统存储空间不足，并保留当前语言。
Guest locale 在 AppSession 创建时确定，因此确认切换时须先停止并回收 AppSession；Host 可以在会话之间
更新 locale、字体和界面，不需要重启设备。所有字体代理更新与绘制由 LVGL adapter lock 串行化。

## 字符集

第一版使用固定 DeepSeek-V4-Flash tokenizer 提取的完整 UTF-8 字符、各语言基础字表和对应语言 UI 必需字符，
再与 Noto 的 cmap 求交集。字节级 BPE token 先反解后提取，不能直接读取 token 的表面字符串。
生成时验证全部 UI 必需字形，移除 hinting；不承诺任意文本或完整 Unicode 覆盖。
基础字表采用 GB2312 一级汉字、Big5 一级汉字、JIS X 0208 一级汉字及假名、KS X 1001 的 2,350 个韩语音节和 ASCII。它们保证基础覆盖，不等同于语言的全部字符；新增 UI 文案仍自动并入。后续通过字体组件版本扩充。

在独立 Python 环境安装 [固定工具版本](../../tools/fonts/requirements.txt)，提供固定版本的
Noto 静态 TTF 和 DeepSeek tokenizer（Unicode 数据版本 16.0.0）。输入 SHA-256 不匹配时拒绝生成。

```sh
python tools/fonts/build_language_fonts.py \
  --deepseek-tokenizer /path/to/deepseek-tokenizer.json \
  --sources /path/to/noto-regular-fonts \
  --output build/language-fonts
```

输入字体来自 [Noto](https://github.com/notofonts/noto-cjk)。字体产物保留 OFL 授权；
固件正常构建不下载 tokenizer 或原始大字体。增加翻译必须重新检查字形覆盖。

生成器会在输出目录的各 Locale 子目录生成 `app.json`、资源清单、TTF 和许可证；用 `python3 tools/micropixel publish <目录> --dry-run` 验证，再去掉 `--dry-run` 发布。设备通过 `GET /device/v1/devices/{deviceId}/fonts/locales/{locale}` 查询版本、签名和相对下载地址，并仅通过同一 Control QUIC 连接获取 Bundle。选择其他语言时查询对应组件的可用版本；网络不可用时允许使用已安装版本。组件在商店中默认隐藏，勾选“显示系统字体”后可浏览、下载；安装与更新由设备语言菜单完成，以确保字体和语言共同生效。

## Guest 字体前置条件

固定使用某种语言的 App 可在 `requirements` 声明 `"system_font": "zh-CN"`（支持上述五种 locale）。
这是启动前置条件，和 App 的翻译列表不同。`en` 使用内置拉丁字体，其他值要求当前生效的系统字体
与该 locale 一致；安装 Bundle 不会自动修改设备语言。缺少字体时 Host 拒绝创建 Guest 会话，
以模态错误弹窗明确显示所需字体名称和设置中的目标语言。弹窗正文可滚动，关闭按钮固定在屏幕内；关闭后清除本次 Hall 错误提示，诊断记录仍保留。没有该字段的旧 App 保持原行为。
该声明保证语言字体可用，不代表完整 Unicode 覆盖；固定 UI 文案应在发布前与字体子集验证。

语言确认前仅查询元数据。确认后 Host 先停止并回收当前 AppSession，再准备字体、下载和安装；
停止失败时拒绝继续。取消确认不退出 App。字体成功激活后，各系统页面和 Hall 共享新的显示 locale。
