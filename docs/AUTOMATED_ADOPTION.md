# 自动化接入判断（三层）

日期：2026-11-18。目的：把「何处接入 Memory」从开发者逐点判断转为
机器分类、影子估算与策略校验。**三层只产出推荐与证据，不切换任何
运行时路径，不构成性能资格**；arena 晋升仍须按
[BASELINE_PROTOCOL.md](BASELINE_PROTOCOL.md) 做同工作量 A1/A3 配对采样。

## Layer 1：静态分类器

入口 `tools/classify_allocation_sites.py`，输入站点清单
[ALLOCATION_SITES.json](../ALLOCATION_SITES.json)（源码级事实：kind、
尺寸形态、寿命、已有复用、是否堆分配等），输出三态分类：
具体候选（`arena_candidate`/`system_candidate`/`copyto_candidate`/
`import_candidate`）、`excluded` 或 `uncertain`。

规则要点（fail-closed，顺序即优先级）：

1. 已有复用池（BIL P1-07/P2-05 等）、GPU owner、无堆分配 → `excluded`，
   防止把既有优化重复记作 Memory 增量。
2. 双端已是 lease 的传输 → `copyto_candidate`；外部 owner 可稳定借用的
   单缓冲 → `import_candidate`。
3. 单块 dense、长命、需稳定地址、逃逸异步 → `system_candidate`
   （arena 省不下 provider 调用，或整 chunk 被钉住）。
4. 同形、短命成批（≥8 次/批）小块 → `arena_candidate` 并给出建议
   chunk（按对齐后批总字节向上圆整到页，上限 256 KiB，禁止默认 64 KiB）。
5. 混合尺寸或缺分布 → `uncertain`，必须进 Layer 2，不得直接推荐。

当前清单由人工按 [BIL 候选去重表](BIL_CPU_CANDIDATES_00572940.md) 与本仓
消费者维护；用 codegraph 从源码自动抽取事实是后续可选增强，未实现。

## Layer 2：影子估算

入口 `tools/shadow_estimate.py`。输入 JSONL 分配轨迹
（`{"bytes": N, "alignment": A}`，`{"op": "reset"}` 分隔 epoch；epoch 内
分配视为存活至 epoch 结束，对应 staged 成本模型）。Arena 侧复刻
`src/mgmem.cpp` 的 first-fit bump 规则：按序扫描 chunk、跳过 backing
对齐不足者、在 `round_up(used, alignment)` 处放置、否则新发布
`max(chunk, bytes)` 的 chunk；`--prepared-alignment` 对应预备 chunk 档。

输出只含 provider 次数与逻辑 backing（峰值、padding、同形率、窗口判定），
`latency_estimate` 恒为 null：延迟不由模型编造，宿主历史比率仅作
`reference_host_ratios` 背景。推荐 arena 的充要条件：同形率 ≥0.90、
provider 节省 ≥50%、backing 增幅 ≤5%、连续 3 个 64-分配窗口通过
（窗口不足 3 个时要求全部通过）；超预算或 backing 增幅超限推荐 System；
其余 `uncertain`。模拟已用文档 oracle 校验：128 个 64 B fixed 请求恰 2 个
chunk、预备档 mixed 32 请求 1 个 chunk / 64 请求 2 个 chunk
（[ARENA_LOOKUP.md](../bench/ARENA_LOOKUP.md)、[ARENA_SMALL_COST.md](../bench/ARENA_SMALL_COST.md)）。

## Layer 3：策略文件与校验

策略 [ALLOCATION_POLICY.json](../ALLOCATION_POLICY.json) 逐站点记录采用
模式、rationale 与证据；入口 `tools/validate_allocation_policy.py`
fail-closed 校验：无条目站点默认 System/dense 并列为未覆盖（非错误）；
`arena` 必须同时有 Layer 1 `arena_candidate` 与 SHA-256 匹配的 Layer 2
`arena_recommended` 报告，且 budget ≥ chunk；`uncertain` 站点只允许
`shadow`/`system`；`excluded` 站点不得用 `system` 条目变相接入。校验回执
可作后续配对采样 freeze 的输入，本身不改 BIL 或任何消费者代码。

## 串联命令

```text
python tools/classify_allocation_sites.py --inventory ALLOCATION_SITES.json --output build/adoption/classification.json
python tools/shadow_estimate.py --trace <trace.jsonl> --chunk 4096 --site <site-id> --output build/adoption/shadow-<site>.json
python tools/validate_allocation_policy.py --policy ALLOCATION_POLICY.json --classification build/adoption/classification.json [--shadow <site-id>=build/adoption/shadow-<site>.json] --output build/adoption/receipt.json
```

## 明确不做

- 全局拦截 `malloc`/`new` 自动路由：改变所有权语义，超出合同。
- 活对象原地迁移：已交稳定地址不可搬迁；模式切换只能作用于下一个
  新 domain，旧 domain 按原 owner collect/drain。
- 用模型数填 `LOCAL_MEASURED`、宣称 RSS/PSS/能量或 BIL 端到端收益。
- special placement / page hint 默认启用：维持 NOT_SELECTED。
