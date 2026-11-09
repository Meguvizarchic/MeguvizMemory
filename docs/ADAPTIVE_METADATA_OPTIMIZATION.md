# 自适应元数据与作用域调用（2026-11-09）

本轮从 `e92e777` / 核心 `890e1109…` 继续，处理紧凑句柄表的重复增长、
空槽初始化、System 退役查找与常见路径通知成本。
原有公开 C ABI 1.1、单调 ID、读写排斥、预算、同步 provider 回调和不确定
结果隔离合同保持。没有新增生产依赖。随后进一步消除 Backing 链的动态空 head，
最终采用核心为 `8628ce7f…`；`9aca4b74…` 原型及其全部验证/计数独立保留，未做性能采样。

## 实现分层

| 层 | 实现 | 生命周期与限制 |
|---|---|---|
| 私有 owner/lease 元数据 | 同一个 `HandleTable<Record>`，单条内联，批量平坦表 | ID 不编码槽位置；移动记录不移动 payload；回调解锁后按 ID 重新查找 |
| 批次容量策略 | 归零释放数组，仅留下最多 128 槽的数值提示 | 下一批首条仍内联；第二条才申请预测数组；trim/collect 清除提示 |
| 空槽构造 | 只初始化空槽 ID，插入及 backshift 用 placement new 构造完整记录 | 明确遵循 C++17 对象生命周期；空槽 payload 不可读，不承诺用户载荷初始化 |
| Backing 链与退役 | 私有 `BackingList`，节点内嵌前后指针，容器只有 head/tail | 空链与临时分离链不分配 head；节点地址稳定，归还直接 O(1) 摘链；全局 claim 仍为原 std::list |
| 关闭通知 | 持域锁，仅非 OPEN 时通知 drain | 唯一 wait 在同锁下拒绝 OPEN；不增加 waiter 计数，不改变关闭/poison 唤醒 |
| 可选 C++17 便捷层 | header-only `domain::with_allocation` | 复用既有 owner/WRITE borrow RAII；同步回调，无 `std::function` |

容量提示复用原有 `capacity_` 字段，没有增加 domain 或两个表对象的大小。
这里复用的是数值，空批次不保留数组。较大历史批次后，下一批仅两条记录也
可能申请 128 槽（owner 6144 B / lease 3072 B）；它不是按需精确内存下界。
首次批次仍从小容量增长，不能宣称所有增长或瞬时峰值消失。

`BackingList` 只实现本库需要的尾插、遍历、删除、单节点转移及整链转移。
尾插先完成独立节点分配与构造再链接，转移不分配，不移动对象地址。失败
rollback 的局部 Backing 副本只走既有 provider/claim 清理，不消费其复制链接。
析构只销毁私有元数据，不隐式 unlink 其他链，不删除不确定的全局 claim。
域锁、pending 和 maintenance 继续保护回调解锁期间的所有权。

## 一行同步使用

在已经创建的 `mgmem::domain domain` 上：

```cpp
const auto status = domain.with_allocation(4096, [](void *data, uint64_t bytes) { std::memset(data, 0, static_cast<size_t>(bytes)); });
```

默认对齐为 `alignof(std::max_align_t)`；也可显式传入 `bytes, alignment, callback`。
回调只在 allocate 和 WRITE borrow 都成功后运行，可以返回 `void` 或
`mgmem_status`；状态原样返回，异常在 RAII 清理后传播。清理沿用原有 fail-stop
合同。嵌套调用和在回调中移动 domain 都由现有共享所有权保活。

指针只在同步回调内有效，不能交给晚于回调完成的异步任务；异步仍使用显式
owner/lease。Arena 的 backing 与原 allocate 一样留到 reset/trim/close，回调结束
不自动重置整个域。这个薄包装减少调用样板，没有单独声称减少核心加锁次数。

## 搜索与选型依据

本轮 Exa 共 25 次搜索、250 条请求结果、实际返回 250 条，去重为 221 个 URL。
原始检索回执留在本地证据目录，公开索引保留标题、URL、回执哈希及选择理由。
只把原作者论文、标准文件和官方实现用于技术结论；搜索排序不是性能证据。

- [Go 的 Swiss Table 分段设计](https://go.dev/blog/swisstable)以目录和独立小表限制
  大 map 的单次增长；本库目标的 32/64 条小批次仍会增长，目录也有固定成本。
- [F14 官方设计](https://github.com/facebook/folly/blob/main/folly/container/F14.md)
  明确区分保留容量与释放存储。本轮选择有限数值提示，继续归还空数组，没有
  将容器容量缓存悄悄变成长期常驻内存。
- [Swiss Tables 官方说明](https://abseil.io/about/design/swisstables)通过密集控制
  元数据和 SIMD 筛选候选。它没有自动消除重新分配的峰值，直接加入 SIMD、
  增量目录或另一依赖不足以证明本库短任务整体变快。
- [snmalloc 原作者资料](https://github.com/microsoft/snmalloc/)和
  [BatchIt](https://www.microsoft.com/en-us/research/publication/batchit-optimizing-message-passing-allocators-for-producer-consumer-workloads-an-intellectual-abstract/)
  展示按所有权聚合工作的价值；直接延迟本库 provider 最终归还会改变预算与
  回调可见性，本轮继续同步归还，只消除可证明冗余的查找和通知。
- [P0593R6 §3.4](https://open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html)
  解释旧 union copy 生命周期的文字问题；本轮显式构造 payload，不依赖新标准
  的隐式对象创建规则。
- [Swift SE-0322](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0322-temporary-buffers.md)
  和 [Java Arena](https://docs.oracle.com/en/java/javase/26/docs/api/java.base/java/lang/foreign/Arena.html)
  为同步作用域接口提供参考；没有照搬特定运行时的栈提升或线程限制。
- [Boost.Intrusive 官方文档](https://www.boost.org/doc/libs/latest/doc/html/intrusive.html)
  为节点寿命与容器操作分离提供参考。实现只保留本库实际需要的链操作，没有
  引入 Boost，也没有采用析构自动摘链或依赖地址位的技巧。

保留 SplitMix64 和既有线性探测删除。SlotMap 的索引/代数句柄不能直接替换
公开单调 ID，低位索引和单乘法 hash 的研究保证也不能直接等同于本库线性探测
的最坏性能。没有仅凭算法新颖度替换现有可靠路径。

## 统一验证与证据

最终源码相对 `e92e777` 完成独立 832 进程、416 对、58 组对测。32 项冻结输入
校验通过，从原始 cycle/round 重算全部 P50/P95、吞吐、AB/BA 与前后半，保留
8846 个有定义逐对比值和 546 个零基线 null 比值及绝对差。没有重试、剔样、
混池或借用旧轮 `a1bf94c` 的收益。

| 工作负载 | 完整流程 P50 候选/基线逐对比值中位数 | 观察 |
|---|---:|---|
| 重复小批次 16 组 | **0.79031–0.89988** | 15 组各 8/8 对改善，mixed64 held worker 为 7/8 |
| main fixed/mixed/phase 单线程 | **0.90889 / 0.89976 / 0.86349** | 各 8/8 对改善 |
| tiny fixed / mixed | **0.93298 / 0.96772** | 6/8、5/8 对改善；差中位 −26.17 / −13.67 ns/op |
| System 单域 reverse / shuffled | **0.49865–0.66107** | 四组各 4/4 对改善，包含 1/4 线程 |
| System 单域 forward，1/4 线程 | **0.93635 / 0.82218** | 各 4/4 对改善 |

重复小批次表使用 create 前至 destroy 后的 `round_wall`，不是只引用 hot。
legacy 基准使用其完整 `workflow`，计时范围不与新工具强行等同。worker 的
完整 wall 含线程创建/同步/退出；source、DLL、工具与请求存活合同均已绑定。

反向也属于采用决定：once mixed64 held worker 完整 wall P50/P95 为
**1.24818/1.52955**，差中位 **+357.03/+985.94 ns/op**；前后半和 AB/BA 均保留，
不能无证据归为噪声。cold fixed worker 为 **1.30553/1.38696**，差中位
**+35.75/+45.05 μs/op**；同存活合同的分阶段均值显示 hot 与外部时段都变化，
不据此证明具体因果。

once fixed64 held inline 完整 wall 为 **1.06365/1.09752**（约 +17.19/+31.25
ns/op），mixed64 held inline 为 **1.00619/1.00821**。main fixed 四线程 P50
**1.00774**（+2.90 ns/op），single mixed P50 **1.01019**（+7.81 ns/op），
System32 forward 单线程 P50/P95 **1.01774/1.02816**（+58.64/+94.41 ns/op）。
不能以更快的 release 子段掩盖 System 完整流程的反向。其余所有反向指标及
有利/不利/相等对数都在公开全量摘要中，不宣称所有场景无回归。

两轮各 52 个 Arena 真实计数场景及 4 个原始/计数布局完成。四轮 prepared32
每表数组申请 20→8，default128/batch64 每表 48→13；长重复 batch32 每表
2560→516，batch64 每表 1536→261。stream lease 与 cold1 仍为零数组。空批次
live 数组归零，计数 registry 无错误。System 的计时与功能证据单列，不称其为
本轮新增 System 堆计数。

最终 Windows x64 MSVC Release 原始/计数布局相同：domain 544 B、owner 控制
对象 72 B / 槽 48 B、lease 控制对象 48 B / 槽 24 B 均保持。最终 Backing 包含
链接为 96 B，直接上游请求 96 B，等于基线 80 B 对象加 16 B list 节点的真实
请求；没有沿用原型 88 B 对象/104 B 节点的额外成本。单 chunk 完整生命周期
Backing 元数据申请由 **3 次 288 B 降为 1 次 96 B**，空链及临时分离链不申请 head。

首次表增长仍在，held32/64 全程实际 combined 数组峰值仍为 5376/10752 B。
减少累计申请次数不能改称峰值或 RSS 下降；预测容量可能过配，原型和最终
计数均完整保留。

最终核心/JNI **48/48**、C++ **12/12**、ASan **48/48**、安装后独立 C++ 消费者
**1/1（内部 8 case）**通过；直接容器 **11 项、4,239,358 检查、6 次数组 OOM**，
pending 重定位 **1482 检查**通过。七个 arm64 ELF 与三个 C++ REL objects 完成
NDK29/API27/static/16KiB、依赖、无 RPATH 和 strip 静态检查，没有执行目标。
原型额外 object 回读的 label 错误与独立静态补证分别保存，最终驱动首次通过。

这是针对重复小负载和退役路径的工程采用，没有取得 Android 运行、RSS、能耗
或正式产品性能资格。CPU 频率、亲和性、线程调度和其他活动未隔离。
[全量结果、原型、精确诊断源码及全部反向](../artifacts/adaptive-metadata-20261109/decision.json)。
