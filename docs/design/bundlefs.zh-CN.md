# BundleFS 持久化格式与事务模型

BundleFS 是 MicroPixel `app_store` 分区的专用文件系统。它只保存不可变的 Bundle 文件，不提供目录、
随机覆盖写、块链表或 Guest 可见的 Flash 地址。上层 `AppStore` 负责 Bundle 语义、安装策略和 AppId 校验；
Bundle reader、AOT loader 和 App Hall 只通过 BundleFS 的 `open/read/mmap/replace/remove` 接口访问内容。

## 1. ESP32-P4 v1 几何

当前 `app_store` 分区固定为 24 MiB，BundleFS v1 使用以下布局：

```text
0x000000–0x000fff  Catalog Bank 0，4 KiB
0x001000–0x001fff  Catalog Bank 1，4 KiB
0x002000–0x002fff  Catalog Bank 2，4 KiB
0x003000–0x003fff  Catalog Bank 3，4 KiB
0x004000–0x00ffff  预留元数据，48 KiB
0x010000–0x17fffff 383 个 64 KiB Bundle 数据块
```

第一段 64 KiB 是元数据区域，不等同于 Catalog。v1 只把前 16 KiB 分配给四个 Catalog Bank，剩余
48 KiB 保留给以后其他元数据用途。数据区固定从 64 KiB 边界开始，使 ESP32-P4/S3 的 64 KiB Flash
MMU 页可以直接参与 `spi_flash_mmap_pages()`。未来 ESP32-S31 的 16 KiB 变体使用新的格式几何，不改变
已有 v1 字段的含义。

空间统计以整个 `app_store` 分区为总容量；已用容量包含完整元数据区域、因几何对齐而不可分配的尾部以及
已分配的 Bundle 数据块，空闲容量只包含仍可分配的数据块。数据块计数仍只描述数据区，不包含元数据。

## 2. Catalog Bank

一个 Bank 就是一个 4 KiB Flash 擦除单元，并且只保存一代完整 Catalog；BundleFS 没有 Bank 内 slot。
Catalog 记录未使用的尾部必须写零并参与 CRC，以便未来在新格式版本中安全扩展。

每个 Bank 都是自描述记录，至少包含：

```text
magic, format_version, header_size, record_size
generation
bank_index, bank_count, bank_size
metadata_size, data_offset, data_block_size, partition_size
data_block_count, allocation_cursor
file_count, block_map_count
feature_flags, payload_size
file entries, ordered block map
checksum, commit_marker
```

当前固定值为：

- `bank_count = 4`；
- `bank_size = 4096`；
- `metadata_size = data_offset = 65536`；
- `data_block_size = 65536`；
- `data_block_count = 383`；
- `generation` 为 64 位无符号整数。

每个文件条目保存名称、逻辑大小、content ID、SHA-256、块号表起点和块数。所有文件的物理块号按文件
逻辑顺序集中保存在 Catalog 中；数据块本身没有头部、链表或所有者信息。空闲块集合由所有有效文件的
块号表反推，不另存一份可能失去同步的 bitmap。

挂载时必须验证几何关系，而不是直接信任 Flash 中的计数：

- `bank_count >= 2`，且 `bank_index < bank_count`；
- Bank 范围按 4 KiB 对齐且不能越过 `data_offset`；
- `data_offset` 和 `data_block_size` 满足当前目标的 MMU 映射要求；
- `data_offset + data_block_count * data_block_size <= partition_size`；
- 文件数、块号数量、文件大小和每个块号均在固定容量内；
- 不同文件不能引用同一物理数据块；
- magic、格式版本、commit marker 和 CRC 均有效。

不支持的版本或 MMU 几何应报告 `unsupported format`，不能与 CRC/结构损坏混为同一个错误。普通 Catalog
提交不能改变 Bank 数量或块大小；改变几何必须通过显式格式迁移完成。

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
7. 最后单独写入 `commit_marker`。

挂载时扫描四个 Bank，忽略擦除态、未提交或 CRC 无效的记录，选择 `generation` 最大的有效 Catalog。
不需要镜像同一代 Catalog，也不需要 `retired_marker`：旧 Bank 自然构成掉电回退点。目标 Bank 擦除或写入
期间掉电时，上一个 Bank 仍然完整；commit marker 写入后，新 Catalog 才可见。

每个 4 KiB 扇区按约 10,000 次擦除估算，四 Bank 环形约支持 40,000 次 Catalog 提交。Catalog 只在
安装、升级、卸载或显式维护操作时更新，不承载运行日志或高频状态。

## 4. 数据分配与 mmap

Bundle 是不可变文件。安装和升级使用写时复制：分配足够的空闲 64 KiB 块、按 Bundle 逻辑顺序写入，
验证成功后才提交引用新块号表的 Catalog。卸载只提交删除该文件的新 Catalog；其旧块随后重新成为可
分配空间。分配游标循环推进，使擦除负载分散到整个数据区。

一个 Bundle 的块可以在物理 Flash 上离散分布，因此删除和反复升级不会形成必须整体搬迁的连续 extent
空洞。读取按块号表转换逻辑 offset；mmap 则把有序物理页号交给 `spi_flash_mmap_pages()`，得到连续的
虚拟地址。上层不能获取或持久化物理块号。

更新需要同时保留旧文件和新文件的数据块，所以最坏情况下必须有足够空间容纳完整新版本。这是原子
写时复制的容量成本，不应通过覆盖 active 文件来规避。

## 5. 初始化、格式化与 NVS 边界

全擦除态 `app_store` 首次挂载时生成 generation 1 的空 Catalog，不安装任何生产预置 App。USB 连接代表
开发调试工作流，`flash-apps` 和 `flash-all` 默认生成全新的 BundleFS 镜像并写入 Blocks、Snake、Demo；
该操作会替换整个 Catalog，不能用于保留设备上的既有 App。空 Catalog 烧录仅作为显式格式恢复操作。
正常安装和升级仍使用 BundleFS 写时复制事务。

BundleFS Catalog 完全位于 `app_store`，不使用 `sys_store` 或其他 NVS。擦除系统 NVS 不会卸载 App，
也不会重建或回退 BundleFS Catalog。只有显式格式化 `app_store` 才会清除所有 Bundle 和 Catalog；该操作
必须被视为独立的破坏性恢复操作。
