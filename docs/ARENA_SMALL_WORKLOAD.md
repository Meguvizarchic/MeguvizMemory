# Arena 小负载固定成本与结构实验（2026-11-09）

本轮先测固定成本，再实现单个闲置 lease 节点复用，最后统一验证。
**候选保留为实验，当前默认核心继续使用 `40c7b1f`。** 重复 stream 已观察到
完整生命周期收益，但 System、一次性批量持有和单次操作仍有清楚的反向代价，
不足以作为通用默认无回归的依据。

候选核心提交为本地分支 `codex/arena-lease-reuse-experiment-20261109` 的
`7abf79f`，源 SHA256 为 `da0fc141…`；完整补丁同时随主线证据公开保存。
主线纳入成本工具、配对工具、公开行为回归及全部证据，运行库源码不切换。

## 先定位的固定成本

未修改的基线 `40c7b1f` / `c8be3d52…` 上，先运行 128 个成本诊断进程，
分开记录 stream/stages、inline/worker、cycles1/cycles128；另用计数核心副本
观察实际 allocator 请求。计数副本的耗时没有作为性能结果。

- 一次 32/64 请求的 worker 场景中，创建至 ready 约 37.8–52.3 μs/轮，
  ready 后的 outer−loop 约 14.65–16.15 μs/轮。后者是同轮墙钟/调度观察，
  不是独立 CPU 成本，线程创建也不在 outer 计时内。
- cycles128 的 inline stream 中，allocation 阶段约 46.9–50 ns/op，
  其余热路径约 187.5–217.2 ns/op。单次 worker 的固定成本由更多操作摊销。
- 独立的 stages 生命周期中，borrow 约 46.9–50 ns/op、payload 约
  96.9–121.9、owner release 约 15.6、lease release 约 45.3–50。
  stages 同时持有整批 lease，stream 最多持有一个，二者不能相减归因。
- 实际 rebind 后的 `std::allocator` 计数确认：每个请求都申请并归还一个
  80 字节 allocation 节点和一个 56 字节 lease 节点。数字已扣除 map head
  和准备阶段，不是用成功 API 调用数推算，也不是 CRT/OS heap 或 RSS 测量。

因此本轮选择减少真实 lease 节点申请/释放，未继续尝试比较、提示写回或通知微调。
计时阶段还包括锁、树操作、载荷和计时本身，不能将整个阶段耗时都归给 allocator。

## 结构实现与内存代价

仅在 Arena 中，lease 释放时可用 `map.extract(iterator)` 留下一个闲置
`node_type`；下一次借用取严格递增的新 ID，覆盖 key 和 mapped 后插回同一域。
成功插入后才更新 reader/writer 与公开输出。旧句柄仍失效，物理节点复用不复用
逻辑身份。多个活动 lease 不受这个“一个闲置节点”上限限制。

reset 保留闲节点；quiet trim/collect 释放它，destroy 的 RAII 析构兜底。
释放的是已提取节点，不是仍属于 map 的 sentinel。节点不拥有 backing、provider
或外部 context；回调锁外窗口、pending 和原有不确定所有权隔离合同保持。
System 不启用闲节点，但新增 mode 判断及域字段仍可能产生成本。

本机 Windows x64 MSVC Release 的实际布局/计数为：

| 项目 | 基线 | 候选 |
|---|---:|---:|
| domain 请求大小 | 456 B | 472 B |
| node handle 字段 | 无 | 16 B |
| Arena 最多额外闲置节点 | 无 | 一个 56 B 请求 |
| 128 次 stream 的 lease 工作节点申请 | 128 | 1 |
| 128 次 held 的 lease 工作节点申请 | 128 | 125 |

512 次 stream 同样只申请一个 lease 工作节点，held 为 505 个；这不是批量节点池。
reset 保留一个闲节点，trim/collect 回到 map head 基线，destroy 回到进程 startup
基线。head 的 56 B 与闲置数据节点的 56 B 分开记账。预算原本不包含元数据，
这些请求字节不等于 allocator usable size、RSS 或其他 ABI 的节点尺寸。

## 配对结果与采用决定

实现后统一通过候选核心/JNI 46/46、C++ 12/12、ASan 46/46；新增两组公开行为
回归覆盖 fresh/stale ID、权限、reset/trim、外部只读输入、多 reader 跨线程释放
与 CLOSING 最终归零。同一新测试程序加载旧核心也通过，未新增公开 ABI。
五个 arm64 ELF 已交叉构建并通过静态检查，没有运行设备程序。

随后只执行一份冻结的 832 进程、416 对、58 组计划。每组 AB/BA 平衡，
基线与候选使用同一可执行文件、请求和生命周期；无补样、剔样或混池。
新版诊断增加从 domain create 前至 destroy 后的完整 `round_wall_ns`，
纳入延后至关闭的节点释放，避免仅以 hot 改善当作净收益。

| 范围 | candidate/baseline 配对结果 | 解释 |
|---|---|---|
| cycles128，stream 全部 8 组 | 完整 round-wall P50 0.8785–0.9395 | 每组 8/8 改善，前后半同向，逐对差约 −29.04 至 −17.72 ns/op |
| once mixed，batch32，stages worker | 完整 wall P50/P95 1.1221/1.2016 | 6/8、7/8 变慢，两半均不利 |
| once mixed，batch64，stages worker | 完整 wall P50/P95 1.0855/1.1454 | 保留完整反向结果 |
| 单次 mixed，batch1，worker | 完整 wall P50 1.0759，后半 1.3369 | 没有节点复用机会，不能只引用 hot |
| System 32 域 reverse，1 线程 | workflow P50/P95 1.0498/1.0481 | 全部 4 对变慢，吞吐也更差 |
| System 32 域 shuffled，4 线程 | workflow P50/P95 1.0600/1.0320 | 同上 |
| legacy Arena main fixed，4 线程 | allocation P50 1.1182，workflow P50 0.9791 | 分配批次仍有代价，不由流程改善抵消或隐藏 |

其他一次性、held、legacy single 尾部和所有 System 组均在完整报告中。
零基线比值保留为 null，报告同时保留 defined/undefined 个数和绝对差。
全部 native 与另标 derived metric-pair 单元共 8817 个比值有定义、575 个未定义。

本轮不直接切换通用默认实现：结构确实消除了重复 stream 的 allocator churn，
但其收益依赖生命周期，不能将 System 或 held 的代价归为未经证明的噪声。
没有证据把反向结果归因于某条特定指令；本轮也没有再增加候选或补样。

## 证据与复现

[证据索引](../artifacts/arena-small-20261109/decision.json)保留基线成本、真实
allocator 计数、布局、全部 832 进程配对、源码补丁及验证回执。完整原始独立
审计报告以无损 `.json.gz` 保存，并附解压后 SHA256；计数插桩源码仅为诊断，
不进入生产库或 SDK。初始成本工具 v1 的源码也精确归档，未以新增 round-wall
的 v2 控制器重新解释旧数据。

[成本工具合同](../bench/ARENA_SMALL_COST.md)定义计时排除范围：参数解析、
模块定位、初始结果/slot 缓冲分配、JSON 输出以及结束时间戳之后的辅助/结果
容器析构不在 round-wall 内。CPU 频率、调度与其他活动未隔离；结果不取得
Android 运行、产品性能、RSS/能耗、真实 16 KiB loader 或正式发布资格。
