# SDK 多语言测试

使用同一份 Guest 代码展示英语、简体中文、繁体中文、日语和韩语。四种字号和动态计数共用系统字体，不打包字体资源。支持方屏及横屏，包括 SZPI 320×240；布局采用 SDK 短边 720 的逻辑坐标。

`app.json` 的 `localization.translations` 指向五份 `i18n/*.json`。`micropixel package` 校验翻译并生成 `chinese-demo_strings.hpp`。应用通过 SDK 读取系统语言，再选择生成的翻译表：

```cpp
const auto locale = app.localization().CurrentLocale();
const auto strings = chinese_demo_strings::ForLocale(locale);
const char* greeting = strings.Get(chinese_demo_strings::Id::kGreeting);
```

大厅名称来自清单的多语言 `title`，正文来自同一语言的翻译表。计数使用 SDK Timer 和 `Application::Run()` 事件循环。未知语言按目录默认值回退到英语。

这个示例跟随系统当前生效的语言，因此不声明固定 `requirements.system_font`。系统先安装对应字体，再提交语言切换；切换会停止当前 Guest，重新打开示例后读取新语言。固定使用某种语言的应用仍可声明 `system_font`，用于启动前检查。

为原地更新先前的中文示例，保留目录名和 App ID `micropixel.chinese-demo`。

```sh
# S31 / P4
micropixel package guest/apps/chinese-demo --profile release --aot-target riscv32-ilp32f
# SZPI / ESP32-S3，使用匹配的 Xtensa wamrc
micropixel package guest/apps/chinese-demo --profile release --aot-target xtensa
```

验证时，在系统设置切换语言，再从大厅打开“多语言测试”：检查问候语、字号样例、计数文案和底部语言标签。修改翻译后，还需检查文案字符是否包含在对应系统字体子集中。
