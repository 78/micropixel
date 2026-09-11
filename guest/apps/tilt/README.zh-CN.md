# Juicy Tilt

Juicy Tilt 是使用加速度计控制的 100 关滚球迷宫 Guest App。第一关教授基础操作；第二关开始进入
经过物理自动试玩筛选的迷宫，逐步引入脉冲风扇、移动挡板、定时门、锁存压力门和单向回退陷阱。
L8 就会出现较宽松的风扇＋定时门技巧关，L10 是第一次技巧考试，之后每章周期性升级。
最终章的 L93、L96、L99、L100 是 Inferno 版本，需要利用停风和开门的重叠窗口控制入射角与速度。
通关自动解锁下一关，并为每关分别保存最佳时间和精通评级。

开始页可用左右按钮选择已解锁关卡；`RUN FROM 01` 从第一关连续挑战。游戏中点击左上角品牌贴图暂停，
暂停页可继续或重开当前关卡。

生成确定性素材：

```sh
python3 guest/apps/tilt/assets/source/generate_gameplay_assets.py
```

重新生成确定性的 02–100 关。每关默认生成 20 个候选，最终关扩大到 60 个，并自动编译生产物理模型，
在三种相位和控制扰动下试玩后才会写入：

```sh
python3 guest/apps/tilt/assets/source/generate_hard_levels.py --write
```

生成器也能输出任意数量、不直接装入 App 的关卡目录；候选按有限批次编译：

```sh
python3 guest/apps/tilt/assets/source/generate_hard_levels.py \
  --generated-count 24 --output build/tilt-levels-preview.json
```

独立目录从第 2 关开始，仍逐关通过几何、关键路径、全局重复和真实物理门禁。`--checkpoint` 和
`--resume-from` 可用于恢复长时间的批量筛选；配合 `--regenerate-from LEVEL` 可以只重做该关及之后的
目录。

模型与关卡回归通过 `bash tools/tests/test_firmware_host.sh` 运行。

构建正式 Bundle：

```sh
python3 tools/micropixel package guest/apps/tilt --aot-target riscv32-ilp32f
```

## 开发约束

Scene 与物理使用 720×720 逻辑坐标，纹理选用与显示匹配的变体。
碰撞与显示几何统一来自 `assets/source/levels.json`，不能从美术图片推导碰撞边界。
素材生成方式见 [资源说明](assets/source/README.zh-CN.md)。

加速度计样本来自 Host 缓存，`WouldBlock` 时保留最近输入，不阻塞事件循环。
当前 S31 映射翻转 X；P4 轴向仍需确认，不应假定原始传感器坐标就是屏幕坐标。
