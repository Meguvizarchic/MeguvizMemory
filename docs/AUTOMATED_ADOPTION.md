# 判断一个分配点是否适合迁移

先找出谁申请这块内存、谁最后访问它，以及同一批数据什么时候才能全部归还。仓库的 Python 工具可以根据这些事实和分配轨迹比较 System 与 Arena，输出供代码评审的建议。运行工具本身不会改变应用的分配方式。

## 用仓库自带的数据试一次

在仓库根目录运行以下命令。`--out-dir` 必须是尚不存在的目录，再跑一次时换一个名字。

```sh
python tools/run_adoption.py --inventory ALLOCATION_SITES.json --policy ALLOCATION_POLICY.json --out-dir build/adoption-demo
```

`classification.json` 给出每个分配点的分类和理由，`receipt.json` 列出哪些策略通过了校验。想了解 chunk 大小怎样影响内存占用，可以单独扫描随仓库提供的 256 次固定尺寸分配样例。

```sh
python tools/shadow_estimate.py --trace tools/examples/fixed-256.jsonl --sweep
```

这里的轨迹是样例。`recommended_chunk_bytes` 仅适用于输入的轨迹和门槛，`backing` 是底层请求容量，`provider` 次数是向底层申请存储的次数。要判断应用的耗时、RSS 或能耗，需要在目标设备上测量。

## 录入自己的分配点

从[清单样例](../ALLOCATION_SITES.json)复制一个接近的条目。给分配点取一个稳定的 `id`，填写内存来源、大小与对齐、每批次数、使用期限及现有复用方式。尺寸分布不清楚时填 `unknown`，留待采集。下例为每批共同释放的 256 个 64 字节临时块。保存为 `sites.json` 即可运行下一节的命令。

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

已有 Memory 调用的工程可以先运行 `python tools/draft_inventory.py --root consumers --output inventory.draft.json`（将 `consumers` 换成待检查目录）。扫描器按调用名称找线索，可能匹配同名方法或定义，也会漏掉普通数组和其他库的分配。逐项核对 `needs_review`，并手工补入尚未迁移的缓冲区。

| 分类 | 下一步 |
|---|---|
| `excluded` | 沿用当前实现。常见原因是已有复用池、资源属于 GPU，或这个调用根本没有堆分配。 |
| `system_candidate` | 用 System 管理独立寿命或需稳定地址的缓冲区。 |
| `arena_candidate` | 采集真实批次，比较 chunk 大小和峰值容量。 |
| `import_candidate` | 确认外部 owner 能保留完整 backing 和稳定地址，再考虑导入。[外部导入](EXTERNAL_IMPORT.md) |
| `copyto_candidate` | 两端已是 lease 时，可直接 `copyTo`。[Java 传输接口](ANDROID_NATIVE_BORROW.md) |
| `uncertain` | 补齐缺失的寿命或分布信息，先保留原路径。策略可选择 `shadow` 或 `system`。 |

## 用轨迹生成策略

用上面的 `sites.json` 和示例轨迹，可以得到一份可检查的策略草案。评估真实模块时替换 `--trace` 的文件，并保持等号左侧与清单中的 `id` 一致。

```sh
python tools/run_adoption.py --inventory sites.json --trace tile-scratch=tools/examples/fixed-256.jsonl --out-dir build/adoption-tile
```

实际采集应在应用的分配接口处记录字节数、对齐和批次边界。`{"op":"reset"}` 表示一批结束。块会提前归还时，用带 `id` 的 `alloc` / `free` 事件反映真实存活峰值。只有大小的旧格式把同批申请视为一直存活到批次结束。Arena 在 `reset` 前保留已释放的空间。仓库提供可嵌入消费者的[轨迹记录器](../tools/trace_recorder.hpp)。

查看 `shadow-tile-scratch.json` 中的 `verdict`、`explanation` 和 `recommended_chunk_bytes`，再检查 `policy-draft.json` 给出的预算。`arena` 策略要求静态分类为 `arena_candidate`、轨迹报告为 `arena_recommended`、报告摘要匹配，且预算至少覆盖一个 chunk。把草案中的 `rationale` 换成业务依据。可选的 `owner`、`review_by` 用于日后复核。[现有策略样例](../ALLOCATION_POLICY.json)

用 `--policy` 传入定稿后可重新校验。有错误时先看 `receipt.json` 的 `error_summary`。如输出目录中出现 `INCOMPLETE.json`，说明执行中断，应在新目录重跑。未列入策略的站点按默认 System 记录。

默认影子估算要求同形率 ≥90%、底层申请次数减少 ≥50%、峰值 backing 增幅 ≤5%，并检查连续窗口。`--preset` 或 `--thresholds` 可设置适合业务的门槛，实际使用的值在报告的 `config.thresholds`。有真实域快照后，可用 `tools/snapshot_feedback.py` 对照预测值。若明显偏离，先检查采集的批次和寿命是否具有代表性。

## 把策略用到代码里

在下一个新域启用所选策略，旧域由原持有者收尾。先用相同输入比较输出，再检查提交失败、取消和关闭时的归还行为。若准备比较性能，两侧应处理相同字节、产生相同结果，并计入初始化和最终清理。[配对测量方法](BASELINE_PROTOCOL.md) · [项目迁移技能](../.agents/skills/migrate-to-meguvizmemory/SKILL.md)
