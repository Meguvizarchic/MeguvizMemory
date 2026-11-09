# 性能路径与接入（2026-11-09）

先完成同一工作量与所有权语义，再比较耗时。宿主微基准、Android 运行和真实消费者净收益分别记录；本轮不接入 BIL。

## 已选择的实现

| 路径 | 调用与作用 | 验证口径 |
|---|---|---|
| Arena 外部输入 | C 的 `mgmem_import_external_arena_v1` / checked 版本，或 C++ `domain::import_external_arena` | 同域预算计入完整输入与自有工作区；[DEX 示例](DEX_EXTERNAL_IMPORT.md)已完成安装版宿主完整查询 |
| 已有 lease 间传输 | Java `source.copyTo(destination, sourceOffset, destinationOffset, count)` | 每次最多 1 MiB，目标必须可写，重叠按 memmove 处理；须部署配套 Java/JNI |
| Arena 同形及支配请求查找 | 普通 `allocate` 调用，无需新参数 | 域内保留 first-fit；尺寸与对齐均不小于提示请求时复用下界，仍有域锁、metadata 和 provider 成本 |

```java
// source / destination 是已获得并由调用方持有的 ManagedMemoryDomain.Lease。
// 异步工作应先取得 lease 再提交，任务结束再关闭 lease。
source.copyTo(destination, 0, 0, byteCount);
```

关闭 domain 会停止新准入；已有 lease 延续到任务完成。请在合适的清理线程完成原有关闭确认，详见[托管域合同](ANDROID_MANAGED_DOMAIN.md)与[借用合同](ANDROID_NATIVE_BORROW.md)。Java/JNI 使用同批制品；旧 JNI 缺少 nativeTransfer 时不支持新方法。

## 当前 mixed 优化与小负载限制

小负载后续轮已完成固定成本拆分与单 lease 节点复用结构实验。重复 stream 的
完整生命周期有改善，但 System/held/单次反向代价仍在，候选未切换默认核心。
主线继续使用 `40c7b1f` 的实现；[诊断、实现与全部取舍](ARENA_SMALL_WORKLOAD.md)。

已采用请求支配关系下的查找下界复用。首批独立八对中，main mixed 1/4 线程
allocation P50 比为 0.6604/0.4951，workflow 比为 0.8966/0.7610；tiny fixed
workflow 仍有 1.0268 倍、约 +10.55 ns/op 的配对代价。通知和 map 提示没有
进入当前源码。1248 个进程的三批结果分别保留，不混池；新增真正单块的
prepared-single profile，不把原有 2～4 块 tiny 当作单块。
[采用决定与全部限制](ARENA_MIXED_OPTIMIZATION.md)。小负载收益仍未证实。

## 第一轮研究结论

[mimalloc 原论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf)通过 page-local 分片和规律的快慢路径组织减少常见分配成本。[LLVM BumpPtrAllocator 源码](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/Support/Allocator.h)将当前 slab 的常见分配保持简短。对本库的可落地启发是减少同形请求重复寻找 backing；不能将它们不同的释放/重置合同直接替换进本库。

[snmalloc 原论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2020/04/snmalloc.pdf)与[BatchIt 论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/preprint_batchit.pdf)说明按所有权分组的跨线程归还能减少消息与同步成本。直接延后当前核心的最终释放会改变回调时间和预算可见性，因此本轮先保留同步合同；显式异步批次是后续独立候选。

[Android JNI 指南](https://developer.android.com/ndk/guides/jni-tips)强调减少跨语言传输和封送。现有 read/write 已使用 Region API；新增 copyTo 针对两端已有 Memory lease 的情况消除 Java 中转。当前锁与回调路径继续使用普通 JNI，未套用有额外限制的 CriticalNative。

## 本轮实测与适用范围

比值均为候选/基线的配对中位数，小于1表示本轮观察耗时更低，不能用各侧独立中位数相除替代。

| 宿主负载 | 配对耗时比 | 口径 |
|---|---:|---|
| 64 KiB lease 传输，12 对 | 0.32531 | 复用 Java 数组作基线，单个真实宿主 JVM |
| Arena 同形多块，1 线程 allocation P50 | 0.53379 | workflow P50 比0.84624 |
| Arena 同形多块，4 线程 allocation P50 | 0.32488 | workflow P50 比0.68045 |
| Arena 混合多块，1/4线程 allocation P50 | 1.00641 / 0.99436 | 未显示稳定收益 |

首次小容量轮中混合 allocation 比1.64310；独立预声明确认轮增加预热和测量，未稳定复现该退化，但 mixed workflow P50 比仍为1.01767，后半组尾部变慢。全部轮次保留，不承诺无回归。小容量实际为同形2块、混合4块，尚未测真实单块。CPU/频率和其他活动未隔离；这些是合成探索，无产品或 Android 性能资格。

[lease证据](../artifacts/lease-transfer-20261109/host.json) · [Arena全轮次证据](../artifacts/arena-lookup-20261109/host.json)。

## 下一轮研究队列

1. 对同形、混合和单块请求分别测量，检查共享域线程数与尾延迟，决定游标是否值得保留。
2. 相同字节与校验下比较复用 Java byte[] 的 read/write 与 copyTo，避免用重复分配数组人为放大收益。
3. 分析全运行库范围登记的线性查找和域内 metadata 成本，再决定直接节点定位或有界索引。
4. 评估关闭 worker 中 25 ms 轮询改通知等待的价值；依靠实测关闭分布决定实现，不混入当前数据。

原始本机日志、工具身份和路径留在忽略目录；公开结果引用具体提交和工作负载。论文中的收益数字不作为本库实测数据。

## 历史结论（988ea2c）：shape 调校否决，稳定登记引用保留实验

Android 本轮 tiny mixed 的 allocation P50 四对均变慢，应优先检查混合路径开销。现有 ARM64 代码生成在成功复用路径保留3条store，写入 valid、bytes、alignment、iterator 四个字段；当前不同shape成功时也会执行。这是现有策略的合法写入，不能仅凭指令数认定其解释了退化。

保留已选 shape 的候选已实现并单独验证；阶段变化 A→长串 B 且不触发 provider 的反例出现明显退化，因此否决。随后减少提示写入的候选也未证明总体改善，两个补丁和全部结果均保留，主线缓存策略未改。

后续稳定登记节点引用 v2 虽在逆序／打散释放中降低完整流程耗时，arm64 Arena mixed 四线程 workflow 配对比仍为 1.378，tiny fixed 为 1.420。主线继续保留 token 查找；候选存于实验分支，不引入默认开关。宿主 v1、v2 和 arm64 v2 各 160 进程分别独立审计，不混池；并发 mixed 的峰值 backing 差异也保留，不能将全部代价归因于索引指令。见[采用决定及限制](CLAIM_REFERENCE_EXPERIMENT.md)与[完整证据索引](../artifacts/claim-release-20261109/decision.json)。

方向参考[LLVM BumpPtrAllocator源码](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/Support/Allocator.h)与[mimalloc原论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf)的简短快路径与维护组织；没有移植它们不同的释放和预算语义。
