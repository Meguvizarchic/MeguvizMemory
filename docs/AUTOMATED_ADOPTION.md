# 判断一个分配点是否适合迁移

先理清谁申请这块内存、谁最后访问它，以及同一批数据什么时候才能全部归还。仓库提供了一组 Python 工具，可以根据静态清单和分配轨迹对比 System 与 Arena，生成供评审的策略报告。运行工具不会改变应用的运行时路径。

## 用仓库自带的数据试一次

在仓库根目录执行以下命令。`--out-dir` 必须指向尚不存在的新目录，重复运行时请更换目录名。

```sh
python tools/run_adoption.py --inventory ALLOCATION_SITES.json --policy ALLOCATION_POLICY.json --out-dir build/adoption-demo
```

`classification.json` 会输出每个分配点的分类与理由，`receipt.json` 记录策略校验结果。想单独查看 `chunk` 尺寸对内存占用的影响，可对随仓库提供的 256 次固定尺寸样例做扫描：

```sh
python tools/shadow_estimate.py --trace tools/examples/fixed-256.jsonl --sweep
```

这里的轨迹仅作示例演示。`recommended_chunk_bytes` 只针对输入轨迹与阈值有效，其中 `backing` 是底层申请容量，`provider` 次数是向底层申请存储的次数。真实耗时、进程 RSS 或能耗必须在目标设备上实测。

## 录入自己的分配点

可从 [清单样例](../ALLOCATION_SITES.json) 复制接近的条目，为分配点设置稳定的 `id`，补充内存来源、尺寸与对齐、每批次数、生命周期及现有复用方式。尺寸分布不明确时填写 `unknown`，留待采集。下面是一个每批共同释放 256 个 64 字节临时块的示例，保存为 `sites.json` 即可用于下一步分析：

```json
{
  "schema_version": 1,
  "sites": [{
    "id": "tile-scratch",
    "kind": "heap_blocks",
    "shape": "fixed",
    "lifetime": "short_batch",
    "size_bytes": { "typical": 64 },
    "alignment": 16,
    "calls_per_batch": 256
  }]
}
```

已有 Memory 调用的工程，可先运行 `python tools/draft_inventory.py --root consumers --output inventory.draft.json`（将 `consumers` 替换为目标目录）。扫描器按 API 名称匹配线索，可能会命中同名方法或声明，也会漏掉普通数组和其他库的分配，需要逐项核对 `needs_review` 并补入未迁移的缓冲区。

| 分类 | 下一步 |
|---|---|
| `excluded` | 保留原实现。常见原因是已有复用池、资源属于 GPU，或该处本身不产生堆分配。 |
| `system_candidate` | 用 System 管理独立生命周期或要求地址稳定的缓冲区。 |
| `arena_candidate` | 采集真实批次轨迹，比较不同 `chunk` 尺寸下的申请次数与峰值容量。 |
| `import_candidate` | 确认外部 owner 能保活完整 backing 并保持地址稳定，再评估导入。[外部导入](EXTERNAL_IMPORT.md) |
| `copyto_candidate` | 两端都已是 lease 时，直接使用 `copyTo`。[Java 传输接口](ANDROID_NATIVE_BORROW.md) |
| `uncertain` | 补齐生命周期或尺寸分布后再决策，策略中可先标记为 `shadow` 或 `system`。 |

## 用轨迹生成策略

结合上面的 `sites.json` 与示例轨迹，可以生成策略草案。评估真实模块时，将 `--trace` 替换为实际采集文件，并确保等号左侧与清单 `id` 一致：

```sh
python tools/run_adoption.py --inventory sites.json --trace tile-scratch=tools/examples/fixed-256.jsonl --out-dir build/adoption-tile
```

实际采集应在分配接口处记录字节数、对齐和批次边界，用 `{"op":"reset"}` 标记一批结束。如果块会提前归还，使用带 `id` 的 `alloc` / `free` 事件以反映真实并发峰值；仅记录大小的旧格式会假定同批申请一直存活到批末。注意 Arena 在 `reset` 前会保留已归还的空间。仓库提供了可嵌入消费方的 [轨迹记录器](../tools/trace_recorder.hpp)。

依次核对 `shadow-tile-scratch.json` 中的 `verdict`、`explanation` 与 `recommended_chunk_bytes`，以及 `policy-draft.json` 生成的预算。选用 `arena` 策略需同时满足：静态分类为 `arena_candidate`、轨迹结论为 `arena_recommended`、摘要哈希匹配，且预算至少覆盖一个 `chunk`。将草案中的 `rationale` 替换为业务依据，并按需填写 `owner` 和 `review_by`。[现有策略样例](../ALLOCATION_POLICY.json)

策略定稿后用 `--policy` 传入并重新校验。报错时先查看 `receipt.json` 中的 `error_summary`；若输出目录生成了 `INCOMPLETE.json`，表示执行中断，需更换新目录重跑。未列入策略的站点默认按 System 处理。

默认影子估算门槛为：同形率 ≥ 90%、底层申请次数降幅 ≥ 50%、峰值 backing 增幅 ≤ 5%，同时检查连续窗口。可通过 `--preset` 或 `--thresholds` 调整门槛，实际生效值会写入报告的 `config.thresholds`。接入后采集真实域快照，可用 `tools/snapshot_feedback.py` 校验预测精度；若误差较大，先排查采样批次和生命周期是否漏记并发存活。

## 把策略用到代码里

切入代码时只在下一个新建域启用新策略，存量旧域仍由原持有者自然收尾。先用相同输入比对输出字节，再检查提交失败、取消与关闭阶段的归还顺序。若需对比性能，双侧应处理相同数据、输出一致结果，并将初始化与最终清理一并计入。[配对测量方法](BASELINE_PROTOCOL.md) · [MeguvizMemory 接入与迁移技能](../.agents/skills/meguviz-memory/SKILL.md)
