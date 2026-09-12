# BundleFS 持久化格式与事务模型

BundleFS 是 MicroPixel 存放 Bundle 的专用文件系统。它只保存不可变的 Bundle 文件，不提供目录、
随机覆盖写、块链表或 Guest 可见的 Flash 地址。上层 `AppStore` 负责 Bundle 语义、安装策略和 AppId 校验，
并且是唯一直接调用 BundleFS `Open/List/Replace/Remove` 的模块。Bundle reader、AOT loader 和 App Hall
不直接依赖 BundleFS：它们通过 `runtime/bundle/bundle_source.h` 定义的 Bundle source 契约读取内容，
BundleFS 文件由 `runtime/bundlefs/bundle_store_source.hpp` 包装成一个 source（契约见
[架构文档](architecture.zh-CN.md)第 7 节）。

### 实例与介质

`runtime::BundleFs` 是一个实例（实现 `runtime::BundleStore` 抽象接口），构造时绑定一个
`device::BlockStorage` 介质：介质只提供 `Read/Program/Erase/Sync` 和可选的 `Map/Unmap`，并报告几何
（容量、擦除单元、编程单元、是否可映射）。BundleFS 不知道介质是 NOR 分区、SPI NAND 还是 TF 卡；
它把所有几何检查建立在 `BlockStorage` 报告的值之上，Platform 提供适配器：

- `platform/storage/partition_block_storage` 包装一个 ESP 分区（NOR），可映射（`spi_flash_mmap_pages`）；
- `platform/storage/spi_nand_block_storage` 包装 `espressif/spi_nand_flash` + dhara FTL（ESP-Mosaico 的
  128 MiB NAND），不可映射，擦除按扇区 trim（读回 0xFF），事务结束时 `Sync()` 刷写 FTL 日志。

一台设备可以同时挂载多个 BundleFS 实例。`AppStore` 持有一个系统商店（NOR `app_store`，存放系统组件
与出厂 App）和一个可选的 App 商店（板载大容量介质）：组件始终写入系统商店；有 App 商店时，安装和升级
的 App 写入 App 商店，并在提交后清退系统商店中的同名旧副本；两个 Catalog 合并展示（AppId 重复时以
App 商店为准）。App 商店介质损坏或缺席时只影响它上面的 App，系统商店仍然可用。

## 1. v3 几何：由介质决定，由 Catalog 自描述

BundleFS 没有写死的块大小、块数上限或单文件块数上限。格式化时几何由介质报告的值推导，并完整记录在
Catalog header 中；挂载时总是采用已提交 Catalog 记录的几何，只验证它与介质自洽。格式本身不定义
"默认块大小"（`bundlefs_format.h` 只保留 `MICROPIXEL_BUNDLEFS_LEGACY_DATA_BLOCK_SIZE` 用于导入 v1/v2）。

数据块大小 `data_block_size` 的确定规则（`BundleFs::DefaultDataBlockSize`）：

1. 起点是介质自身的单位：擦除单元 `erase_size`；可映射介质再取与 MMU 映射对齐 `map_alignment` 的较大者
   （ESP32-P4/S31 的 NOR 为 64 KiB）；
2. 块号表每块占 4 字节，常驻 RAM。只有当整个介质的块号表超过
   `CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB`（默认 1024 KiB）时才把块大小翻倍，直到落入预算；
3. 板级可以在构造 `BundleFs` 时显式指定块大小（等价于 `mkfs -b`，通过
   `BoardRegistration::SetAppStorage(storage, bundle_block_size)` 传入），只用于格式化；
4. 块大小必须是擦除单元和编程单元的整数倍；编程单元不能大于 4 字节的 commit marker。

元数据区在数据区之前，由四个 Catalog Bank 组成。Bank 大小为
`round_up(max(16 KiB, record_size), erase_size)`，其中 `record_size` 是容纳 `entry_capacity`（50）个
文件条目和整个介质块号表的完整记录；`data_offset = round_up(max(64 KiB, 4 × bank_size), data_block_size)`。
`BundleFs::PlanGeometry` 在块数与元数据大小之间迭代直到收敛，测试用它给出期望值而不是常量。

当前介质的结果：

| 介质 | 报告几何 | 块大小 | 布局 |
|---|---|---|---|
| ESP32-P4 NOR `app_store` 24 MiB | erase 4 KiB，映射对齐 64 KiB | 64 KiB | 4 × 16 KiB Bank，数据区 `0x10000` 起，383 块（与 v2 完全相同） |
| ESP-Mosaico NOR `app_store` 8 MiB | 同上 | 64 KiB | 4 × 16 KiB Bank，127 块 |
| ESP-Mosaico SPI NAND 128 MiB（dhara FTL，约 114 MiB 可用） | 2 KiB 逻辑扇区，不可映射 | 4 KiB（板级显式指定，两个 FTL 扇区） | 4 × 约 120 KiB Bank，数据区约 488 KiB 起，约 29 000 块 |
| 64 GB TF 卡（未来） | 512 B 扇区 | 256 KiB（由 RAM 预算决定） | 由 `PlanGeometry` 推导 |

NOR 上一个 64 KiB 数据块对应 P4 的一个 MMU page 或 S31 的四个连续 16 KiB page；BundleFS 按目标
MMU page size 展开离散块号表，因此 mmap 不要求改变 Catalog、块号或 Bundle 磁盘格式。

挂载时如果 Catalog 记录的几何在当前介质上不成立（块大小不是擦除/编程单元倍数、数据区越界），
或板级显式块大小与已提交 Catalog 不一致，报告 `unsupported format`；改变块大小只能通过重新格式化。

空间统计以整个介质为总容量；已用容量包含完整元数据区域、因几何对齐而不可分配的尾部以及
已分配的 Bundle 数据块，空闲容量只包含仍可分配的数据块。数据块计数仍只描述数据区，不包含元数据。

## 2. Catalog Bank

一个 Bank 是擦除单元对齐的连续记录，只保存一代完整 Catalog；BundleFS 没有 Bank 内 slot。
Catalog 记录未使用的尾部必须写零并参与 CRC，以便未来在新格式版本中安全扩展。

每个 Bank 是自描述记录，v3 header 为 64 字节：

```text
magic, format_version (3), header_size, record_size
generation (u64)
bank_index, bank_count, bank_size
data_offset, data_block_size, partition_size (u64)
data_block_count (u32), allocation_cursor
file_count, entry_capacity, block_map_count (u32)
file entries (120 B each), ordered block map (u32 each)
checksum, commit_marker
```

当前固定值为：

- `bank_count = 4`；
- `entry_capacity = 50`（`BUNDLEFS_MAX_FILES`，结构性槽位数）；
- `generation` 为 64 位无符号整数；

其余字段（`bank_size`、`data_offset`、`data_block_size`、`data_block_count`）由第 1 节的规则在格式化时
确定。单个文件可以占用任意多个数据块，只受块号表总容量限制。

每个文件条目保存名称、逻辑大小、content ID、SHA-256、块号表起点和块数。所有文件的物理块号按文件
逻辑顺序集中保存在 Catalog 中；数据块本身没有头部、链表或所有者信息。空闲块集合由所有有效文件的
块号表反推，不另存一份可能失去同步的 bitmap。

挂载时必须验证几何关系，而不是直接信任 Flash 中的计数：

- `bank_count >= 2`，且 `bank_index < bank_count`；
- Bank 范围按擦除单元对齐且不能越过 `data_offset`；
- `data_offset` 和 `data_block_size` 满足介质报告的擦除单元、编程单元与映射对齐要求；
- `data_offset + data_block_count * data_block_size <= partition_size`；
- 文件数、块号数量、文件大小和每个块号均在 header 声明的容量内，且 header 声明的容量能放进 Bank；
- 不同文件不能引用同一物理数据块；
- magic、格式版本、commit marker 和 CRC 均有效。

不支持的版本或几何应报告 `unsupported format`，不能与 CRC/结构损坏混为同一个错误。普通 Catalog
提交不能改变 Bank 数量或块大小；改变几何必须通过显式格式迁移完成。挂载成功后 Catalog 常驻 RAM
（受互斥量保护），读路径不再重新扫描 Flash；提交失败会丢弃缓存，下一次操作重新扫描。

## 3. 环形提交与掉电恢复

四个 Bank 按 `bank_index` 环形更新：

```text
generation 1 -> Bank 0
generation 2 -> Bank 1
generation 3 -> Bank 2
generation 4 -> Bank 3
generation 5 -> erase Bank 0, then write Bank 0
```

提交新 Catalog 时：

1. 新 Bundle 数据先写入尚未被 active Catalog 引用的数据块；
2. 逐块读回并完成 Bundle/SHA-256 校验；
3. 选择当前 Bank 的下一个 Bank；
4. 擦除目标 Bank；
5. 写入 Catalog header、payload 和 checksum；
6. 读回并验证；
7. 最后单独写入 `commit_marker`；
8. 调用介质 `Sync()`，让带 FTL 日志的介质（NAND）把整个事务落盘；NOR 的 `Sync()` 是空操作。
   每次 Catalog 提交只需要一次 `Sync()`，数据块写入本身不刷写。

挂载时扫描四个 Bank，忽略擦除态、未提交或 CRC 无效的记录，选择 `generation` 最大的有效 Catalog。
不需要镜像同一代 Catalog，也不需要 `retired_marker`：旧 Bank 自然构成掉电回退点。目标 Bank 擦除或写入
期间掉电时，上一个 Bank 仍然完整；commit marker 写入后，新 Catalog 才可见。

每个 4 KiB NOR 扇区按约 10,000 次擦除估算；一个 Bank 的所有扇区同步擦除，四 Bank 环形仍约支持
40,000 次 Catalog 提交。Catalog 只在安装、升级、卸载或显式维护操作时更新，不承载运行日志或高频状态。

挂载器兼容读取旧 v1（四个 4 KiB Bank、最多 7 个文件）和 v2（四个 16 KiB Bank、最多 50 个文件、
64 KiB 数据块）。扫描顺序是：介质起点 header 提示的 Bank 大小 → 为该介质规划的 Bank 大小 →
16 KiB 环 → v2 → v1。发现旧版本后先在内存中导入为 v3 记录（块大小沿用 Catalog 记录的 64 KiB，
日志标记 `migration pending`），文件与块号表保持不变；第一次新事务把 v3 记录写入 16 KiB 环的下一个
Bank，不会擦除仍持有旧 Catalog 的回退 Bank。v3 提交成功后继续环形更新，因此升级固件不会要求先格式化
或丢失已有 App。

Catalog entry 的逻辑顺序同时作为 App Hall 的展示顺序。正常事务安装一个新 App 时将其插入 index 0；
卸载时保留其余 App 的相对顺序。BundleFS 的 `bundlefs_begin_replace` 在替换同名文件时保留原 index，但
App Store 重装已安装 App 的流程是先 `bundlefs_remove` 再安装（见第 4 节），因此重装后的 App 也会出现在
index 0。物理数据块仍可循环利用、离散分配，而用户总能在大厅最左侧找到刚安装或刚重装的 App。离线构建 App Store 镜像时，命令行 Bundle 参数顺序就是展示
顺序。

## 4. 数据分配与 mmap

Bundle 是不可变文件。安装使用写时复制：分配足够的空闲数据块、按 Bundle 逻辑顺序写入，
验证成功后才提交引用新块号表的 Catalog。卸载只提交删除该文件的新 Catalog；其旧块随后重新成为可
分配空间。分配游标循环推进，使擦除负载分散到整个数据区。

一个 Bundle 的块可以在物理 Flash 上离散分布，因此删除和反复升级不会形成必须整体搬迁的连续 extent
空洞。读取按块号表转换逻辑 offset；mmap 则把有序物理页号交给 `spi_flash_mmap_pages()`，得到连续的
虚拟地址。上层不能获取或持久化物理块号。

Bundle reader 从不把整包读入 RAM。`micropixel_open_aot_package` 只读取 TOC，并把 AOT 段复制到
PSRAM（校验哈希并检查 XIP）；其他段在被使用时才通过 `micropixel_bundle_open_asset/open_font` 逐段
打开并校验哈希：贴图在 `LoadTexture` 期间打开、解码或复制后立即关闭；字体在会话内缓存映射（LVGL
就地读取字形），音频剪辑在剪辑存活期间持有映射。安装校验 `micropixel_validate_app_package` 以 4 KiB
分块流式计算所有段的哈希。可映射介质（NOR）上一段就是一个 mmap 窗口；不可映射介质（NAND）上
`Map()` 返回 `unavailable`，source 只提供 `read`，reader 把单个段读入 PSRAM 副本（日志中标记
`ram-copy`）。副本没有人为大小上限，PSRAM 不足时在加载点失败并记录请求字节数。因此几十 MiB 的
Bundle 只占用当前正在使用的资源所需的内存。

大厅封面只在大厅阶段保留局部映射。启动或恢复 App 时，Host 先保留独立的启动画面/Guest 画面，
暂停封面加载并释放封面映射，再加载整包；返回大厅时重新打开封面（挂起 App 使用快照）。
封面集合同时记录跳过的快照 App；在大厅停止该 App、快照索引清除后，重新打开它的静态封面。
否则离散 Bundle 中的某段可能已被封面完整映射，`spi_flash_mmap_pages()` 会返回
`ESP_ERR_INVALID_STATE`，导致安装校验成功但启动时整包映射失败。该约束与屏幕分辨率无关。

App Store 重装（安装 AppId 已存在但 digest 不同的包）不使用"新旧版本并存"的原子升级，而是先卸载
再安装：`InstallApp` 先确认新包能放进"当前空闲空间 + 旧版本占用的块"，再提交删除旧文件的 Catalog，
最后按普通安装流程写入并提交新文件。这样重装只需要容纳新版本本身的空间，代价是删除后到新 Catalog
提交前的任何失败（写入错误、包校验失败、掉电）都会让该 App 处于未安装状态，需要重新安装；digest 相同
的重复安装仍是无写入的 no-op。Host 在进入安装流程前已要求没有正在运行或挂起的 AppSession，因此旧块
不会在被映射期间被回收。BundleFS 本身仍不允许覆盖 active 文件的数据块。

## 5. 初始化、格式化与 NVS 边界

全擦除态介质首次挂载时生成 generation 1 的空 Catalog，不安装任何生产预置 App。介质含有非擦除态
内容但没有任何 BundleFS magic（例如出厂 FAT 镜像）时报告 `not formatted`，与"有 magic 但记录无效"的
`corrupt` 区分；NOR `app_store` 遇到前者不会自动格式化。扩展 App 存储的策略由
`BoardRegistration::SetAppStorage(storage, bundle_block_size, removable)` 的 `removable` 标志决定：
板载固定介质（Mosaico NAND）在 `not formatted` 或 `unsupported format`（例如旧固件留下的另一种块大小）
时由 `FirmwareApp` 自动 `Format()` 后挂载，因为它不承载系统组件；可插拔介质属于用户，固件启动时
永不格式化。`corrupt` 在两类介质上都原样保留。凡未就绪的扩展存储都由 `AppStore` 以
`ExternalStorageState` 报告到目录，系统 UI 的 App 管理页显示"扩展存储未格式化 / 格式不兼容 / 已损坏"
并在用户确认后调用 `AppStore::FormatExternalStore()`；该操作与卸载共用"无 AppSession"前置条件，
成功后重新加载目录。扩展存储未就绪期间，下载的 App 回退安装到系统存储。`Format()` 只擦除元数据区，
NAND 上约 0.3 s。USB 连接代表
开发调试工作流，`flash-apps` 和 `flash-all` 默认生成全新的 BundleFS 镜像并写入五个示例 App；
该操作会替换整个 Catalog，不能用于保留设备上的既有 App。空 Catalog 烧录仅作为显式格式恢复操作。
正常安装和升级仍使用 BundleFS 写时复制事务。

BundleFS Catalog 完全位于 `app_store`，不使用 `sys_store` 或其他 NVS。擦除系统 NVS 不会卸载 App，
也不会重建或回退 BundleFS Catalog。只有显式格式化 `app_store` 才会清除所有 Bundle 和 Catalog；该操作
必须被视为独立的破坏性恢复操作。

### 商店版本替换

App Store 更新保留旧文件，以 `begin_replace` 写入新副本，校验完成后提交 Catalog；不得为了腾出
空间先卸载旧应用。空间不足或提交前失败保留旧版本，成功替换保留原 Catalog 顺序与应用存档。
发布身份、能力要求与远程队列见[应用商店契约](app-store.zh-CN.md)。
