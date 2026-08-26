# 定时器与大厅空闲功耗

本文记录产品固件中会周期唤醒 CPU 的 LVGL timer、`esp_timer` 和主要 FreeRTOS 超时等待。第三方协议栈
内部定时器不逐项展开；`esp_timer_get_time()` 只是读取单调时钟，不代表创建了定时器。

## LVGL 定时器

| 来源 | 周期 | 功能 | 空闲策略 |
|---|---:|---|---|
| LVGL tick clock | 按需读取 | LVGL 9.5 通过 tick callback 读取 `esp_timer_get_time()` | 产品不再创建 1 ms periodic tick timer；时间只在 LVGL 查询时读取 |
| Display refresh timer | 1000 ms | 检查 dirty area 并提交 LCD 刷新 | 静态画面不靠它轮询刷新；Host/Guest 修改 UI 时通过 `RequestDisplayRefresh()` 将其置为 ready 并唤醒 adapter |
| Host pointer read timer | LVGL 默认 4 ms | 系统菜单、状态层、Wi-Fi 页面和大厅的 pointer/scroll/long-press 处理 | 使用 `LV_INDEV_MODE_EVENT`；无触摸时暂停，触摸样本到达时恢复并置为 ready，释放后再次暂停 |
| LVGL animation timer | LVGL 默认 4 ms | LVGL 内建动画 | 没有 animation 时由 LVGL 自身暂停；当前 Host 主要转场由 PPA/有限帧循环完成 |

`esp_lv_adapter` 的 worker 按 `lv_timer_handler()` 返回的下一个 deadline 等待，最长兜底 120 s；等待还会被
1 s auto-sleep deadline 约束，进入 idle pause 后无限阻塞，直到触摸 IRQ、Guest frame、Host UI 更新或其他
显式 wake。GT911 IRQ 只唤醒输入读取任务；确认得到有效触摸样本后，Host 输入路径才唤醒 LVGL，避免空 IRQ
造成 adapter 的 idle pause 反复退出。产品启用 ESP-IDF PM 和启动时 DFS：任务活跃时仍可运行在 360 MHz，空闲时降到 XTAL 频率。
当前没有启用 FreeRTOS tickless automatic light sleep。

## 用户可配置的自动休眠

System Settings 的 Power Management 页面使用 LVGL `switch` 和 `dropdown` 配置空闲休眠。默认超时为
5 分钟，可选 1、5、10、30 分钟，也可关闭。该策略不是 FreeRTOS tickless automatic light sleep：它在
Host supervisor 的现有事件等待上追加一个 deadline，到期后产生与短按电源键相同的 Host 入睡请求，继续复用
App 安全暂停、背光渐暗、显示释放、显式 `esp_light_sleep_start()` 和电源键唤醒流程。

计时只在外接电源状态明确为未连接时进行；供电状态未知或 USB/无线供电已连接时不会自动休眠。拔掉外接电源、
修改设置或从 light sleep 唤醒都会开始一轮新的倒计时。物理触摸、系统手势和 Remote Control 注入的触摸/按键
都会刷新最后交互时间。固件更新期间到期的请求按现有电源保护规则拒绝，不中断 OTA 事务。设置以向后兼容的
v2 Host settings record 保存在 `sys_store/system`；旧 v1 record 首次读取时采用 5 分钟默认值。

## 与 pocket-sage 事件循环的对应

`pocket-sage/main/gui/gui_manager.cc` 的核心做法是调用 `lv_timer_handler()` 后，按其返回的下一个 LVGL
deadline 等待 EventGroup；没有 timer 时最长等待 120 s，画面或输入变化再设置 `GUI_WAKE_BIT`。本项目继续使用
`esp_lv_adapter` 管理 MIPI-DSI、flush 和 PM 生命周期，没有复制一套 GUI task；adapter fork 新增 LVGL 9
monotonic tick mode，并用 `esp_lv_adapter_request_wake()`/ISR 版本实现相同的“按需时钟 + deadline + 外部事件”
语义。

## 显式创建的 `esp_timer`

| 所有者 | 数量/周期 | 功能 | 结论 |
|---|---|---|---|
| LVGL adapter | 产品模式为 0 个 | LVGL tick 改成按需读取单调时钟 | 已删除产品的 1 ms periodic tick；adapter 默认 periodic 模式仍为其他项目和 LVGL 8 保持兼容 |
| Guest `TimerService` | 每 Session 最多 8 个，Guest 指定 one-shot/periodic | 将 Timer 到期转换成统一 Guest event | 保留。App suspend 时全部停止，periodic event 会合并，且设置 `skip_unhandled_events` |
| Wi-Fi discovery | 1 个 one-shot | 用户扫描后 20 s holdoff；失败后按 60 s、120 s、300 s、900 s退避发现已保存网络 | 保留。它本身就是 deadline/event 模型，不是固定轮询，并设置 `skip_unhandled_events` |

集成 Guest 的周期定时器只在对应 App 前台运行：Blocks 与 Snake 为 16,667 us（约 60 Hz），Demo Timer 页为
100 ms，Demo atlas 页为 20 ms。它们负责游戏推进或演示，不影响 App Hall 空闲。

## 其他周期唤醒和超时等待

| 路径 | 周期/超时 | 当前处理 |
|---|---:|---|
| App Hall 状态兜底采样 | 30 s | Wi-Fi、外接电源和远控命令均由事件立即唤醒；30 s只用于电量/固件状态兜底。电量滤波按实际流逝时间补权重，不会因采样变稀而把原约 60 s窗口拉长到 30 min |
| 性能浮层 | CPU sample 1 s | 仅用户显式打开浮层时启用；等待 UI/远程事件或下一采样 deadline，不再 20 ms 轮询 |
| Remote Host command | 旧实现 Poll 250 ms | 已改为入队时通知 `SystemShell`；仅远控 input sequence 执行期间保留 250 ms deadline 推进 |
| Resource decode worker | 旧实现 Queue Poll 20 ms | 已改为 `portMAX_DELAY` 阻塞；shutdown 通过队列 sentinel 唤醒 |
| Wi-Fi 扫描页 | retry 1 s，刷新 10 s | 只在扫描页面可见时按下一个 deadline 等待；Wi-Fi driver 状态变化仍走事件 |
| Remote agent 离线状态 | 1 s | 仍存在；用于未配置、无网络和 disabled 状态的重试/固件检查，是下一阶段事件化候选 |
| Remote agent 已连接 control stream | Read timeout 250 ms | 仍存在；用于同时推进网络流、Host result 和本地控制命令。应给 HTTP/3 stream 增加可由本地队列打断的等待，再把常态 timeout 放大或改成无限等待 |
| 音频 I2S mixer | 旧实现每 128 帧写一次，16 kHz 下约 8 ms | 已改为 task notification 事件唤醒；无 active voice 时关闭 PA 和 I2S 并无限阻塞，播放结束只保留 80 ms 静音 grace |
| 前台 App completion | 20 ms | 仅 Guest 前台期间，用于 completion、远控和系统动作编排；不是大厅空闲来源 |
| 固件更新页面 | 100 ms | 仅更新页面/更新流程期间刷新进度；可在 Remote model change event 完整接入后删除 |
| 亮度与系统转场 | 15–17 ms，约 100–180 ms 总时长 | 有限帧瞬态任务，结束后不再唤醒 |

## 本轮优化边界

本轮优先消除了 App Hall 的持续唤醒来源，同时保持屏幕常亮和触摸即响应：

1. LVGL 9.5 使用按需单调时钟，从根上取消 1 ms tick timer；1 s 无活动后 adapter pause；
2. pointer 从 4 ms永久轮询改为触摸事件驱动；
3. 所有会改变画面的 Host/Guest 路径统一执行 refresh-ready + adapter wake；
4. 远控命令从 250 ms轮询改为队列事件；
5. 大厅状态兜底采样从 1 s调整到 30 s；
6. Guest resource worker 从 20 ms轮询改为阻塞队列；
7. 音频 mixer 从持续发送静音改为首个 tone 事件启动、最后一个 tone 结束后自动关闭 I2S/PA。

暂不启用 tickless automatic light sleep。启用前必须在 Metalio-Claw4 真机逐项验证 MIPI-DSI、PPA、PSRAM、
ESP-Hosted SDIO、GT911 和电源键在自动睡眠中的 retention/wake 行为。下一阶段应先把 Remote agent 的
250 ms control stream timeout、1 s离线轮询和固件状态刷新改成网络/队列/Remote model event，再评估
tickless light sleep。

后续可单独评估把 `CONFIG_FREERTOS_HZ` 从 100 提升到 1000，以获得 1 ms 的阻塞和 deadline 粒度；这不是
LVGL 动画流畅度修复的前置条件。若采用 1000 Hz，应同时启用并验证 tickless idle，并把 LVGL 动画显示提交
独立限制在约 16–20 ms（50–60 FPS），避免当前 4 ms animation timer 实际触发约 250 次/秒的无效刷新。
验收必须包含活动态 Tick ISR/CPU 开销、大厅待机功耗、ESP-Hosted SDIO 抖动，以及显式 light sleep 的进入和
唤醒稳定性。

## 真机验收

- 启动日志应显示 monotonic tick mode，`esp_timer_dump()` 中不再出现 `LVGL tick` periodic timer；
- 大厅静置 1 s后应看到 LVGL adapter 进入 auto sleep，且无 4 ms pointer 周期唤醒；
- 触摸大厅、打开/操作/关闭系统 UI、Guest 连续渲染、远控截图/输入均能即时唤醒且不丢首帧；
- 性能浮层关闭时记录各任务 runtime delta；重点观察 `lvgl`、`micropixel_assets` 和 Host supervisor；
- 对比改造前后 60 s大厅静置的平均电流、CPU 频率驻留和唤醒次数；
- 验证 30 s电量兜底刷新、USB/无线供电插拔即时刷新，以及 Wi-Fi 状态事件即时刷新；
- Power Management 关闭时不应自动进入 light sleep；开启后，仅在未接外部电源且达到所选空闲时间时进入；
- 插入 USB/无线供电应暂停自动休眠，拔出后重新完整计时；唤醒后也应重新完整计时；
- 自动休眠与短按电源键应走同一显式 light sleep 流程，前台 App 唤醒后恢复原 Session；
- 固件更新期间即使达到空闲 deadline，也不得进入休眠。
