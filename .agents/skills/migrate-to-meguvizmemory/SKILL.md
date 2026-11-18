---
name: migrate-to-meguvizmemory
description: Migrate an allocation site or codebase to MeguvizMemory end to end — inventory, classification, shadow estimation, policy, code rewrite, and validation gates — without claiming unmeasured performance gains.
---

# 一站式迁移到 MeguvizMemory

在 MeguvizMemory 仓库内（或任何已 vendoring 本仓 `tools/` 的工作区）使用本 skill，
把「要不要、把何处迁到 Memory 依赖」走完一条可验证链路：盘点 → 分类 → 影子 → 策略 →
改码 → 验证 → 汇报。所有命令默认在仓库根目录执行，产物一律落新目录（`build/` 下）。

## 硬规则（任何一步不得违反）

1. 不接管进程全局 allocator（不替换 `malloc`/`new`）；迁移只发生在显式域内。
2. 不搬迁已交出稳定地址的活 allocation；模式切换只作用于下一个新 domain，
   旧 domain 按原 owner `collect/drain`。
3. lease 必须在异步任务入队前取得；指针对 lease 外不得逃逸（延寿靠持有 lease 或复制到独立 owner）。
4. `excluded`（已有复用池/GPU/无堆分配）不得变相接入；`uncertain` 先影子/实测，不得直进 arena。
   已有优化（如 BIL P1-07/P2-05）的收益永不记作 Memory 增量。
5. backing 是逻辑请求容量，不是 RSS/PSS；provider 次数下降不等于加速。
   未做同工作量 A1/A3 配对采样前，不得宣称性能、内存或能耗收益（见 `docs/BASELINE_PROTOCOL.md`）。
6. chunk 禁止默认拍脑袋 64 KiB：用分类建议或影子扫描推荐值，小输入配大 chunk 是已记录回归。
7. 失败即停并如实汇报：校验 FAIL、影子 verdict 非推荐、快照 DRIFT 时不迁该站点，只报告原因与补证路径。

## 零步：确认依赖可用

先确认目标工程已能链接 Memory：CMake 侧 `find_package(MeguvizMemory)` 链接
`MeguvizMemory::mgmem`，或 Android 侧坐标 `io.github.jichuo1.meguviz:mgmem-android`
（版本以根 `CMakeLists.txt` 与 `bindings/android/` 为准，不凭记忆写版本号）。
链接不通时先修构建，不进入迁移。

## 第一步：盘点站点（生成清单草案）

```text
python tools/draft_inventory.py --root <源码目录...> --output build/migrate/inventory.draft.json
```

逐条人工补齐每条的 `needs_review`（confirm_call_site、kind、shape、lifetime、
existing_optimization、calls_per_batch，以及标出的 size_bytes）。拿不准就填 `unknown`，
不要猜——未知会被分类为 uncertain 并转入影子，这是预期路径。定稿保存为正式清单
（仓内既有种子是根目录 `ALLOCATION_SITES.json`，新站点可先放 `build/` 再决定是否入库）。

## 第二步：分类（决定每站动作）

```text
python tools/classify_allocation_sites.py --inventory <清单.json> --output build/migrate/classification.json
```

| 分类结果 | 动作 |
|---|---|
| excluded | 不迁，记录理由结束 |
| system_candidate | 可迁 System 域（收益在所有权与记账，不在 provider 次数） |
| arena_candidate | 进第三步影子，推荐后才可写 arena 策略 |
| import_candidate | 用外部导入借用已有 owner，不复制 |
| copyto_candidate | 双端 lease 改直传 `copyTo` |
| uncertain | 进第三步；证据仍不足则保持现状 |

## 第三步：影子估算（arena 候选与 uncertain 必经）

拿轨迹的优先序：真实轨迹 > 样例轨迹。真实轨迹在消费者 API 层记录
（`tools/trace_recorder.hpp`：alloc/free/reset，测完 `close()` 再读）；
没有真实负载时用样例先探形状：

```text
python tools/make_trace.py --pattern fixed|mixed|stream|staged --count 4096 --output build/migrate/t.jsonl
python tools/shadow_estimate.py --trace <t.jsonl> --site <站点id> --sweep [--preset conservative] --output build/migrate/shadow-<站点id>.json
```

读报告只看四件事：`verdict`、中文 `explanation`、`recommended_chunk_bytes`、
`counterfactual`（没达标时最近达标点）。注意 `lifetime_model`：含显式 free 按 stream
同时存活峰值判增幅，与 staged 全批口径不可混比。只有 `arena_recommended` 可进 arena 策略。

## 第四步：策略（机器回填，人只写理由）

```text
python tools/generate_policy_draft.py --classification build/migrate/classification.json [--shadow 站点id=build/migrate/shadow-站点id.json] --output build/migrate/policy.json
```

逐条把自动 rationale 换成真实依据，补 `owner` 与 `review_by`（ISO 日期）。
或者一步到位用一键入口串完二至四步（产物目录必须不存在，失败看其中 `INCOMPLETE.json`）：

```text
python tools/run_adoption.py --inventory <清单.json> [--policy <策略.json>] [--trace 站点id=<t.jsonl>] [--preset conservative] --out-dir build/migrate/run1
```

校验未 PASS 不进第五步；`error_summary` 给聚合原因，warnings 是复审卫生问题（告警不阻断但要处理）。

## 第五步：按分类改码（只用已核实 API）

C++（`<mgmem/memory.hpp>`，细节与边界见 `docs/CPP_CONVENIENCE.md`；
状态均为 `[[nodiscard]]`，逐个判断、失败保持旧对象可用）：

```cpp
// System 域：单大块、长命、需逐个归还
mgmem::domain memory;
auto st = mgmem::domain::create_system(budget_bytes, memory);
// 一次同步工作区：自动归还 owner 与 WRITE lease，指针不得逃逸回调
st = memory.with_allocation(bytes, [](void *data, std::uint64_t n) { /* 使用 data */ });
// 保存 owner 多次访问
mgmem::owned_allocation owner;
st = memory.allocate(bytes, owner);
st = owner.with_write([](void *data, std::uint64_t n) { /* 写入（默认未初始化） */ });
st = owner.with_read([](const void *data, std::uint64_t n) { /* 读取 */ });
```

```cpp
// Arena 域：仅限影子推荐且策略已定的同形短命批次
mgmem::domain arena;
auto st = mgmem::domain::create_arena(budget_bytes, recommended_chunk_bytes, arena);
// 批内反复 allocate/with_allocation；整批结束、无活 lease 时才可复位复用：
// 在 C 层用 mgmem_arena_reset_v1，顺序与状态机见 docs/ADR-001-ownership-and-abi.md。
```

外部已有 owner（如预读 vector）可借用时走 import 而不是复制：C 的
`mgmem_import_external_v1` / Arena 变体 `mgmem_import_external_arena_v1`
（须先解析符号并查 `MGMEM_CAP_ARENA_EXTERNAL_IMPORT`），合同见 `docs/EXTERNAL_IMPORT.md`。
迁移增长场景记得：无隐式 realloc，先申请新 backing、迁移、再释放旧的，预算覆盖共存峰值。

Java/Android 托管域（合同见 `docs/ANDROID_MANAGED_DOMAIN.md`，分配先清零）：

```java
ManagedMemoryDomain domain = ManagedMemoryDomain.arena(budgetBytes, chunkBytes); // 或 .system(budget)
ManagedMemoryDomain.Allocation allocation = domain.allocate(bytes, alignment);
try (ManagedMemoryDomain.Lease lease = allocation.borrowWrite(0, bytes)) { /* 使用 */ }
// 双端都是已持有 Lease 时直传，避免 Java 中转数组（单次上限 1 MiB）：
source.copyTo(destination, sourceOffset, destinationOffset, count);
// 消费者合同就是 ByteBuffer 时，用作用域直视图（零拷贝；缓冲不得逃逸回调，异步交接禁用）：
lease.withDirectBuffer(0, bytes, buffer -> { buffer.order(java.nio.ByteOrder.nativeOrder()); /* 同步消费 */ });
```

关闭顺序不可省：停新准入 → 等全部 allocation/lease 结束 → 在清理线程 collect → drain → destroy；
持有自身 lease 的线程不得等自己。Java/JNI 必须同批制品，旧 JNI 不支持新方法。

## 第六步：验证（缺一不可，且不越界宣称）

1. 功能等价：原工作量完整跑完且输出与迁移前 oracle 逐项对齐（checksum/字节级）；失败路径
   （预算拒绝、非法输入）仍显式报错；关闭后快照 backing/lease 为 0（`domain.observe()`）。
2. 生命周期：异步任务在 lease 取得后入队；owner 先释放时已借出 lease 仍可读完；
   reset/trim 只在无活资源时发生。
3. 模型回核：拿真实域快照与影子预测对账，DRIFT 就重采集轨迹再估：

```text
python tools/snapshot_feedback.py --shadow build/migrate/shadow-<站点id>.json --snapshot <快照.json> --mode arena
```

4. 仓内离线套件不回归：`python tools/test_classify_allocation_sites.py` 等六套接入测试，
   以及被改模块自有测试。需要性能结论时另起正式配对采样（独立 pilot、冻结样本与门槛），
   本 skill 到此为止——只交付「功能等价 + 证据齐全」，不交付收益数字。

## 常见失败与处置

| 症状 | 处置 |
|---|---|
| 分类多为 uncertain | 缺的是分布证据，先补真实轨迹，不要放宽门槛凑推荐 |
| 影子 growth 超限 | 看 `counterfactual` 换小 chunk 重估；仍超限则保持 System |
| 策略校验 FAIL | 按 `error_summary` 修：错配分类、缺影子、SHA 对不上（报告被改后需重生成草案） |
| 运行期 `OUT_OF_BUDGET` | 预算要覆盖共存峰值与 arena 保留量，不是只覆盖 live 字节 |
| 关闭卡在 BUSY/drain 超时 | 还有 lease 或异步任务未结束；先停新准入、等任务完，再让清理线程 collect，不要强释 |
| 快照 DRIFT | 负载已变：重录轨迹、重跑影子与策略，不沿用旧推荐 |

## 汇报模板（迁移结束时给用户）

逐站点一行：站点 id、分类、最终模式（system/arena/import/copyto/未迁）、证据文件路径；
随后写明：功能等价如何验证、关闭是否归零、快照是否在容差内、仍属工程预览未做性能资格的部分。
未迁站点与原因同样列出，不得只报迁了的。
