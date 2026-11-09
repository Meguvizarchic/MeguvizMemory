# Arena 混合请求优化与小负载结论（2026-11-09）

本轮采用请求支配关系下的 first-fit 扫描下界复用。**main mixed 有明确宿主改善，
小负载收益尚未证实**：tiny fixed 的 workflow P50 配对中位比为 1.02679，
逐对耗时差中位数约 +10.55 ns/op。该代价随采用决定公开保留。

## 实现与合同

旧实现仅在 bytes、规范化 alignment 完全相同时复用查找位置。现在满足
`bytes >= cached_bytes && alignment >= cached_alignment` 时也可复用。
alignment 均为 2 的幂；更大的对齐不会产生更小的向上对齐偏移，更大的
请求也不会使已失败的前缀重新可用。小请求或更宽松的对齐仍从头 first-fit。

成功及失败扫描仍覆盖当前提示，provider 发布、reset、回收等原有失效规则
保持。没有新增缓存项、域字段、容器、生产依赖或公开 ABI；调用方继续使用
原有 allocate 接口。新增两个指针反例覆盖“尺寸增大但对齐下降”和
“对齐增大但尺寸下降”，避免把两个必要条件误写为任意一个。

通知门控、所有 metadata 尾部提示及仅 allocation 尾部提示均保留实验，
不进入当前生产源码。不同实验区块只能分别与其配对基线比较，不能用各侧
独立中位数或非配对绝对时间给候选排名。

## 采用依据与反向结果

基线为 `988ea2c`，核心 SHA256 为 `744d596c…`；选定核心为
`c8be3d52…`。以下仅引用首批 dominance-only 原始八对，均为逐对
candidate / baseline 耗时比的中位数，小于 1 表示该组观察耗时更低。

| 宿主负载 | allocation 批次 P50 | workflow P50 | 解释 |
|---|---:|---:|---|
| main fixed，1 线程 | 1.0000 | 0.9925 | 未显示明确整体收益 |
| main fixed，4 线程 | 0.9929 | 1.0016 | 保留不利样本 |
| main mixed，1 线程 | 0.6604 | 0.8966 | workflow P50/P95 均 8/8 对改善 |
| main mixed，4 线程 | 0.4951 | 0.7610 | 同上；全部八对峰值 backing 轨迹不同 |
| main phase，1 / 4 线程 | 1.0000 / 0.9816 | 0.9963 / 0.9910 | 独立阶段切换反例 |
| tiny fixed，1 线程 | **1.1370** | **1.0268** | allocation P95 1.2713，workflow 6/8 对变慢 |
| tiny mixed，1 线程 | 1.0370 | 0.9874 | 不据此宣称稳定小负载收益 |
| prepared single fixed，1 线程 | 1.0000 | 0.9979 | 使用已预备的单块 |
| prepared single mixed，1 线程 | 0.9655 | 1.0040 | 保留 workflow 代价 |

main mixed 1 / 4 线程的 workflow P50 逐对差中位数分别约为 −38.35 /
−159.00 ns/op。tiny fixed 的 allocation P50/P95 逐对差为 +10.16 /
+30.47 ns/op，workflow P50/P95 为 +10.55 / +6.25 ns/op。这些是批次或
轮次墙钟时间除以操作数后的统计，不能解释为单 API 调用延迟。

全部三批分别保留：首批 dominance/notify/组合 576 进程，metadata 提示
416 进程，收窄提示 256 进程，共 1248 进程、624 对、96 组，无补样、
剔样或混池。后两批只作为撤出提示策略的独立证据，不继承为选定源码的性能。
例如，第二批组合的 tiny mixed workflow 比 1.029；最后一批为 1.100，
前后半 0.956 / 1.352，说明总体中位数会掩盖明显漂移。

## 小负载覆盖与验证范围

原 tiny 的 128 请求实际上使用 fixed 两块、mixed 四块。新增
`--profile prepared-single`：计时前预备一个 4096 字节、64 对齐块并 reset，
随后仅执行一个线程、32 请求、batch32。原生程序与控制器均检查 provider
前后为 1、峰值 4096、字节 oracle 及最终关闭归零。该 profile 有独立
workload ID，默认 CLI 和原 fixed/mixed/phase 工作量不变。

workflow 不含最终 close；phase 与 prepared-single 也不包含预备和 reset。
这不是冷启动或准备至关闭的净收益。CPU 频率、调度及其他活动未隔离；并发
mixed 的实际容量轨迹不同，不能把全部差异归因于扫描指令，也不能宣称相同
内存成本、RSS/能耗下降或产品性能资格。

选定源码重新通过核心及可选 JNI 45/45、C++ 12/12、ASan 45/45。
扩展 shape suite 为 5 项、586 检查，并在同一测试程序加载基线和候选时通过。
首轮 ASan 因子进程缺少编译器运行库目录而超时；原记录与 0xc0000135
加载器诊断保留，仅修正子进程 PATH 后重跑通过，没有修改全局环境。

选定源码的核心、两个基准及测试程序已完成 arm64 交叉编译与 ELF 静态检查。
16 KiB LOAD 段对齐不等于真实 16 KiB loader 运行；本轮没有设备、APK/AAR、
SDK 发布或 BIL 集成证据。也没有 dominance-only 的 System 性能实测，
其他候选的 System 反向组不能充当它的性能通过证明。

完整逐对结果、尾部、前后半、源码与原始记录摘要见
[证据索引](../artifacts/arena-mixed-20261109/decision.json)；可读分组摘要旁边的
`.json.gz` 是保留全部明细的原始独立审计报告，解压后 SHA256 可按索引核对。
原始可执行文件、日志及全部未采用源码仍留在本机忽略的 `build/`。
