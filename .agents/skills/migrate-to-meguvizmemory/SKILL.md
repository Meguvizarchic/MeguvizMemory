---
name: migrate-to-meguvizmemory
description: 评估现有 C、C++ 或 Android 模块中的 CPU 缓冲区是否适合 MeguvizMemory，并按需迁移分配、借用和关闭逻辑。适用于选择 System 或 Arena、导入已有缓冲区以及替换 lease 间的数组中转。
---

# 迁移一个内存使用场景

从用户指定的模块、分配函数或数据流开始。评估任务给出分配点、建议和依据，迁移任务还要修改调用方并验证结果。以下命令在 MeguvizMemory 仓库根目录运行，扫描目录可以指向使用方工程。

## 找到值得迁移的缓冲区

沿着一个分配点找出申请方、最后一个读写方、归还时机，以及拒绝提交或取消时由谁清理。确认地址是否已交给第三方、对方是否在回调返回后继续使用。已有复用池、栈上数据和 GPU 管理的资源通常保留原实现。

已有 Memory 调用的工程，可先运行 `python tools/draft_inventory.py --root consumers --output inventory.draft.json`，把 `consumers` 换成目标目录。扫描器按名称匹配，需核对 `needs_review`、补入没有使用 Memory 的原分配点。清单和轨迹格式都有可直接运行的例子。[分配点评估指南](../../../docs/AUTOMATED_ADOPTION.md)

## 选择持有方式

| 分配点的特征 | 接入方式 |
|---|---|
| 单块、长寿命，或一批中的对象要独立归还 | 用 System，按稳定地址和所需峰值设定预算。 |
| 小块同形、整批结束，可明确指出何时复位 | 记录真实申请、归还和批次边界，评估 Arena。 |
| 外部来源能保证完整缓冲区地址稳定，且提供保活和释放 | 导入一次，多个范围从同一 owner 借用。[外部导入](../../../docs/EXTERNAL_IMPORT.md) |
| Java 的两端都已有 lease | 用 `copyTo`，目标可写，单次最多 1 MiB。 |
| 第三方同步接受 `ByteBuffer` | 用 `withDirectBuffer`，仅在该回调内使用视图及其切片。对方需要保留缓冲区时，保留原持有方式或提供独立副本。 |
| 已有优化，或尚不清楚大小和寿命 | 记录原因。对缺失的信息采集样本后再决定。 |

用 `run_adoption.py` 可以一次完成分类、轨迹估算与策略校验。把示例文件名和 `tile-scratch` 换成实际清单、站点 ID 和轨迹，`--out-dir` 指向一个新目录。已有策略时附加 `--policy`，没有轨迹的非 Arena 站点可省略 `--trace`。

```sh
python tools/run_adoption.py --inventory sites.json --trace tile-scratch=trace.jsonl --out-dir build/adoption-review
```

查看 `classification.json`、`receipt.json` 及对应的 `shadow-*.json`。采用 Arena 需同时有 `arena_candidate` 分类、SHA-256 与策略一致的 `arena_recommended` 报告，以及容得下 chunk 的预算。按工作负载选择 chunk。未知的分布和超出预算的报告都应回到原分配点核实。样例轨迹只用来熟悉工具。

## 修改调用方

- **C / C++** 通过已安装的 `MeguvizMemory::mgmem` 接入。同步临时空间可用 `with_allocation`。反复访问时持有 owner，跨线程时在入队前取得 lease。检查返回状态，先构造新 backing 再切换 owner，预算覆盖两者共存的峰值。[原生接入](../../../docs/INTEGRATION.md) · [C++ 示例](../../../docs/CPP_CONVENIENCE.md)
- **Android** 核对目标 ABI、minSdk、编译 SDK 和实际 AAR 提供的接口，使用同一套 Java、JNI 与核心库。allocation 和 lease 用 `try` 归还。域的 `beginClose()` 安排后台清理，观察到 `CLOSED` 后再 `forgetClosed()`。需要 CMake 的应用根据制品选 Prefab 或独立 SDK。[Android 快速接入](../../../docs/ANDROID_QUICKSTART.md) · [托管域关闭](../../../docs/ANDROID_MANAGED_DOMAIN.md#关闭内存域)
- 新模式用于新建的域。回退时停止新准入，让旧域沿原路径结束。仍在使用的稳定地址和来源回调都要保留。

## 验证迁移

使用目标模块原有输入比较输出，并走一遍与改动有关的失败路径，例如预算拒绝、异步提交失败或取消。确认最后一次访问先于释放，域在归还后完成关闭。若使用 Arena，用真实快照和轨迹预测核对峰值。偏差较大时检查是否漏记并发存活或批次边界。[快照对照工具](../../../tools/snapshot_feedback.py)

交付时说明哪些分配点改变了、为何这样选择、使用了哪份 SDK 或 AAR、怎样回退，以及实际运行过哪些测试。只有需要给出性能结论时，才在相同设备、输入和完成量下测量。把耗时、底层申请次数、逻辑容量和进程内存分别记录。[配对测量方法](../../../docs/BASELINE_PROTOCOL.md)
