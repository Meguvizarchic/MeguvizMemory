# 性能路径与接入（2026-11-09）

先完成同一工作量与所有权语义，再比较耗时。宿主微基准、Android 运行和真实消费者净收益分别记录；本轮不接入 BIL。

## 已选择的实现

| 路径 | 调用与作用 | 验证口径 |
|---|---|---|
| Arena 外部输入 | C 的 `mgmem_import_external_arena_v1` / checked 版本，或 C++ `domain::import_external_arena` | 同域预算计入完整输入与自有工作区；[DEX 示例](DEX_EXTERNAL_IMPORT.md)已完成安装版宿主完整查询 |
| 已有 lease 间传输 | Java `source.copyTo(destination, sourceOffset, destinationOffset, count)` | 每次最多 1 MiB，目标必须可写，重叠按 memmove 处理；须部署配套 Java/JNI |
| Arena 同形分配查找 | 普通 `allocate` 调用，无需新参数 | 域内保留 first-fit 语义，缓存仅减少重复找块；仍有域锁、metadata 和 provider 成本 |

```java
// source / destination 是已获得并由调用方持有的 ManagedMemoryDomain.Lease。
// 异步工作应先取得 lease 再提交，任务结束再关闭 lease。
source.copyTo(destination, 0, 0, byteCount);
```

关闭 domain 会停止新准入；已有 lease 延续到任务完成。请在合适的清理线程完成原有关闭确认，详见[托管域合同](ANDROID_MANAGED_DOMAIN.md)与[借用合同](ANDROID_NATIVE_BORROW.md)。Java/JNI 使用同批制品；旧 JNI 缺少 nativeTransfer 时不支持新方法。

## 第一轮研究结论

[mimalloc 原论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf)通过 page-local 分片和规律的快慢路径组织减少常见分配成本。[LLVM BumpPtrAllocator 源码](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/Support/Allocator.h)将当前 slab 的常见分配保持简短。对本库的可落地启发是减少同形请求重复寻找 backing；不能将它们不同的释放/重置合同直接替换进本库。

[snmalloc 原论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2020/04/snmalloc.pdf)与[BatchIt 论文](https://www.microsoft.com/en-us/research/wp-content/uploads/2024/05/preprint_batchit.pdf)说明按所有权分组的跨线程归还能减少消息与同步成本。直接延后当前核心的最终释放会改变回调时间和预算可见性，因此本轮先保留同步合同；显式异步批次是后续独立候选。

[Android JNI 指南](https://developer.android.com/ndk/guides/jni-tips)强调减少跨语言传输和封送。现有 read/write 已使用 Region API；新增 copyTo 针对两端已有 Memory lease 的情况消除 Java 中转。当前锁与回调路径继续使用普通 JNI，未套用有额外限制的 CriticalNative。

## 下一轮研究队列

1. 对同形、混合和单块请求分别测量，检查共享域线程数与尾延迟，决定游标是否值得保留。
2. 相同字节与校验下比较复用 Java byte[] 的 read/write 与 copyTo，避免用重复分配数组人为放大收益。
3. 分析全运行库范围登记的线性查找和域内 metadata 成本，再决定直接节点定位或有界索引。
4. 评估关闭 worker 中 25 ms 轮询改通知等待的价值；依靠实测关闭分布决定实现，不混入当前数据。

原始本机日志、工具身份和路径留在忽略目录；公开结果引用具体提交和工作负载。论文中的收益数字不作为本库实测数据。
