# 紧凑句柄表与稳定登记引用（2026-11-09）

> 本文保留 e92e777 阶段的原始实现与实测。当前实现继续采用自适应元数据和侵入式 Backing 链，见[最新增量优化](ADAPTIVE_METADATA_OPTIMIZATION.md)。

本轮采用组合结构：allocation 与 lease 共用私有紧凑句柄表，串行首条记录内联，
批量记录使用连续数组；全局 backing 登记以稳定节点引用定位清理对象。
**Arena tiny、主矩阵和 System 的实际工作流程均取得宿主改善**，同时保留一次性
场景、尾延迟及扩容内存峰值的代价，不承诺任意负载无回归。

## 从固定成本到结构实现

此前计数已确认：旧路径每次请求申请、释放一个 80 B allocation 节点和一个
56 B lease 节点。上一轮单节点复用只能覆盖串行 lease，批量持有仍反复申请。
本轮先实现 lease 表，再依据真实阶段数据继续整合 allocation 表与登记定位。
两版各 832 进程独立保留，不用不同轮次的非配对绝对时间给部件排名。

实现包含：

1. `HandleTable<Record>` 只保存纯标量记录。一个活动记录直接内联；第二条起
   分配四槽数组，按 3/4 occupancy 上限倍增。公开 ID 仍严格递增，不等于槽号。
2. 采用线性探测与回移删除。删除后修复穿过空洞的探测链，不保留 tombstone；
   遇到位于 home 的中间记录也继续扫描，直至真正 empty。平均查找成本依赖
   哈希分布，不是最坏 O(1) 保证。删除消费已找到的 Slot，不再重复定位。
3. 每张表活动记录归零即释放数组，回到空内联状态。allocation 记录包括 pending
   与 retiring 状态；owner 提前归还不等于记录消失。仍有记录时可能持有此前
   较大容量，quiet trim/collect 才能整体清理，不能描述成逐节点即时回收。
4. provider/retain 回调解锁期间只携带 ID 与稳定 Backing iterator，回锁后重新
   查找 allocation。lease 继续保存 allocation ID，不引入跨表 Slot 指针。
5. `Backing` 保存全局 `RangeClaim` 的稳定 list iterator。正常清理直接标记和
   删除自身节点，去掉两次按 token 线性查找；重叠与 external ID 检查仍保留。
   未知回调结果继续充记与隔离，不重试、不隐式删除不确定 claim。

删除思路参考[无 deleted 标记的开放寻址删除研究](https://arxiv.org/abs/0909.2547)，
代码按本库锁与生命周期重写。ID 分散使用原作者公开领域
[SplitMix 输出混合步骤](https://prng.di.unimi.it/splitmix.c)，来源及原声明随 SDK
安装保存；没有引入随机数生成器或新的生产依赖。

## 当前采用的配对证据

基线 `a1bf94c` 的核心为 `c8be3d52…`；组合核心为 `890e1109…`。以下均为
同可执行文件、同请求与生命周期下 candidate / baseline 的逐对耗时比中位数。
组合轮 832 进程、416 对、58 组全部独立从原始 cycle/round 重算。

| 工作负载 | 配对耗时比 | 观察范围 |
|---|---:|---|
| legacy Arena main 六组 workflow P50 | 0.7913–0.8729 | 全部组各 8/8 对改善 |
| tiny fixed workflow P50 | **0.8452** | 8/8 改善；逐对差中位约 −62.89 ns/op |
| tiny mixed workflow P50 | **0.8886** | 8/8 改善；逐对差中位约 −53.13 ns/op |
| System reverse/shuffled 主要组 workflow P50 | **0.4236–0.6034** | 多数组各 4/4 改善，完整分组见报告 |
| System 全部十二组 workflow P50 | 全部组中位比低于 1 | 不代表每一对或全部尾部改善 |

System preparation 包括 WRITE borrow、载荷写读及 lease 归还；prepared checkpoint
和 timed owner release 才是 N owners / 0 leases。不能把整个 System 过程说成
没有 lease，也不能将组合收益归给没有隔离测量的单个部件。

## 必须保留的反向与内存成本

完整时段由 domain create 前计至 destroy 后。以下展示 hot 改善没有全部转化为
完整时段收益的实测，不能只引用 hot：

| 一次性诊断范围 | hot P50 比 | 完整 wall P50/P95 比 | 完整 wall 逐对差中位 ns/op |
|---|---:|---:|---:|
| fixed batch64 stream inline | 0.8684 | **1.0599/1.0946** | +20.31/+31.25 |
| fixed batch64 held inline | 0.9439 | **1.1368/1.1278** | +46.09/+46.88 |
| mixed batch64 held worker | 0.9406 | **1.2280/1.1136** | +314.84/+189.84 |

legacy single mixed 的 allocation P50 仍为 1.1929（约 +17.19 ns/op，7 对慢、1 对
相等），workflow P95 为 1.0541。System 32 域 forward 单线程的 P95 也有
1.0031、约 +8.25 ns/op 的代价。cold fixed inline 的完整 wall P50 为 1.0，P95
为 2.5，不能将其 hot 改善描述为稳定的单请求收益。全部前后半、AB/BA、零基线
比值和绝对差保留。

Windows x64 MSVC Release 原始/计数布局均实测一致：domain 456→544 B，
allocation 控制块 16→72 B、元素 48 B，lease 控制块 16→48 B、元素 24 B；
`Backing` 仍为 80 B。旧两个 map 的动态 head 不再存在。单请求两表都内联，
没有这两类存储的堆申请；**不等于整个 API、backing 或 domain 零分配**。

连续数组减少申请次数，但增长会使新旧数组共存。例如 held32 的实际两表
combined 数组峰值为 5376 B，held64 为 10752 B。不能将各表不同时刻的峰值
直接相加。每类同时最多两份数组，本次单域 fixture 的 combined 峰值为三份；
每批归零后两类 live 数组均为 0。批量重复增长的累计请求字节可以更多。
这些都是上游请求和 host 布局，不能转换为 allocator usable size、RSS 或能耗。

## 正确性与交付范围

组合版核心及可选 JNI **48/48**、C++ **12/12**、ASan **48/48** 通过。
直接表测试 **8 项、4,238,156 检查、4 次分配故障注入**，包括碰撞环绕、
at-home 中间元素、20K seeded 独立 oracle、owner 全字段重定位及两类增长 OOM。
独立 pending relocation 回归在 provider callback 阻塞时继续创建、退役十六个
活跃 lease 的 owner，迫使两表增长；回调恢复后首条记录正确发布与关闭归零。

六个 arm64 ELF 完成交叉编译与 ABI、16 KiB LOAD、依赖、无 RPATH、strip 加载段
检查，没有运行设备程序。未生成/发布新 Android SDK、APK/AAR，未接入 BIL。
公开 C ABI、ID、权限、预算和不确定回调隔离合同保持；内部实现替换不等于正式
产品性能资格。CPU 频率、调度与其他活动未隔离，stream/stages 存活集合不同，
不能跨集合做减法归因，结果容器析构等计时排除项见成本工具合同。

[完整证据索引](../artifacts/handle-table-20261109/decision.json)分别保存首版和
组合版全部结果、真实分配计数、源码补丁、精确诊断源与验证回执。
