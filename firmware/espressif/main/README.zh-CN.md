# Host 源码导航

`FirmwareApp` 是组合根，依赖方向为 `Runtime → Device contracts ← Platform`。

| 目录 | 职责 |
|---|---|
| `runtime/` | Session、Service、Bundle 与 Guest 资源 |
| `device/` | 硬件无关契约与设备身份 |
| `platform/boards/` | 引脚、器件组合、供电和初始化 |
| `platform/drivers/`、`platform/buses/` | 器件驱动与共享总线 |
| `platform/graphics/`、`platform/lvgl/` | 合成、呈现与 LVGL 适配 |
| `platform/storage/` | `device::BlockStorage` 介质适配器（NOR 分区、SPI NAND） |
| `platform/memory/` | PSRAM 分配器、`MICROPIXEL_EXT_RAM_BSS`、`PsramString` / `PsramVector` / `PsramMap` 与 `PsramBuffer` |
| `runtime/bundlefs/`、`runtime/bundle/` | BundleFS 实例、Bundle source 与多商店 `AppStore` |
| `host/controller/` | 应用切换、电源与本地/远程控制 |
| `host/ui/` | 大厅、系统页面、手势与布局 |
| `work/` | 后台执行器与任务策略 |

大厅安装展示由 `host/ui/hall_install_model.hpp` 组织：新 App 的进度卡置于首位，更新已有 App 时保留原位。
进度更新按 App ID 查找展示位置，卡片操作按 App ID 映射回实际目录索引；不改变存储目录。

[架构](../../../docs/design/architecture.zh-CN.md) · [构建](../../../docs/development/flashing.zh-CN.md)

## App 更新请求反馈

App Hall 与 App Management 共用的更新操作弹层在点击 Install 后立即显示请求中状态，
等待期间不再提供安装、运行旧版或卸载操作；Close 只关闭弹层，不取消请求。
安装请求状态独立于 Store 更新检查，关闭后重新打开仍保留等待状态；服务端接受请求后显示等待下载，
下载开始后由 Hall 的安装进度接续。请求失败时显示错误并恢复重试入口。
同一时刻只保留一个未完成的 Store 安装请求，重复点击不能再次提交。

Hall、Manage Apps、Wi-Fi 和 Remote Control 的 Action Sheet 共用 `ActionSheetPresenter`，
复用 Status Layer 的快照合成器与逐帧进度。LVGL 回调只创建弹层并提交合并的显示请求；
`SystemShell::PollAction` 在 Host 任务消费请求，先取得 scanout 再取得 LVGL 锁，完成入场与缓冲释放。
等待期间透明遮罩拦截输入，内容保持隐藏；合成器保留已刷新的页面背景后才显示遮罩与弹层，
以屏幕底部为隐藏位置完成 PPA 转场。请求槽固定为一个，删除对象或离开页面时取消；Shell 的
待处理标志保证动作队列满或重置不会丢失唤醒，展示事件不会传给业务控制器。
P4 与 S31 的 ARGB 快照共用对齐步长校验，PPA 像素行距包含填充，可见区域使用弹层实际宽度。
入场完成后释放临时快照和背景缓冲，恢复普通 LVGL 交互；已显示弹层的内容刷新不重播入场。
无硬件合成器或准备/呈现失败时恢复 LVGL 刷新，使用按弹层实际高度滑入的公共软件动画。

确认卸载时，UI 与 Host 分别锁定一次请求，阻止重复卸载和其他 App 操作。Host 在删除与目录
刷新前同步提交“Uninstalling...”等待画面，并暂停页面触摸输入；成功后返回 Hall 或刷新应用列表，
失败时保留确认弹层并显示错误与重试入口。Manage Apps 的模型刷新复用页面根节点，避免外层
先清空页面造成弹层引用失效。卸载仍由 Host 串行执行，等待画面不提供取消按钮。
