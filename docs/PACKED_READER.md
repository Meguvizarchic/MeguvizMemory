# Packed reader 合同与验证

2026-10-30。MM-010 交付了独立的纯 Java reader，生产产物无 Kotlin 或其他第三方依赖；**BIL 尚未接入，host `parseFrom`、Hook/UI 与性能均未由本模块取得资格**。该组件针对的是普通消费方的逐元素对象实例化开销，不属于 Memory 分配器接入或加速收益。

## 真实输入合同

本模块基于只读审计的 BIL HEAD `00572940c354fbec7b6f311d4ecd6f02e66ea7c5`（验证前后 HEAD 一致，两份 oracle 源码字节 SHA-256 校验不变，路径相对 BIL 仓库根目录）：

| 来源 | SHA256 |
|---|---|
| `Bilibili_Innocent_Lab/app/src/main/java/com/Bilibili_Innocent_Lab/xposedmodule/hook/feature/danmakuwire/DanmakuWire.kt` | `ce221d29a11b1d8b435df751bcd00ba9b173a3b66f949edebcf87ae79c377ae8` |
| 同目录 `DanmakuWireFields.kt` | `00b263ea3e347add784c3eaa8e14e76dc8aa7da5a7c12af53a44da62fe8745a8` |

核实 Rust `flatten`、JNI 交接及真实调用方后，`flat` 布局为 `[N, S, (weight, colorful, flags, contentStart, contentLength) × N, (kind, present) × S]`，不含额外 magic 或版本号；元素 `flags` 的 bit 0 / 1 / 2 分别代表 `weight`、`colorful`、`content` 存在，source 的 `present` 为 `0` 或 `1`。存在标志与数值相互独立，`0`、负数或极值均不作为空值哨兵。

接口严格限制 `N <= 65536`、`S <= 65536`、`reply <= 16 MiB` 并校验精确总长度（长度计算采用 `long`）。负数计数、超限、截断、尾部多余数据、未知标志位、非 `0`/`1` 的 source 标志以及越界内容区间均会被拒绝。针对缺失 `content` 时的原始偏移（Rust 生产者填 `0`、Kotlin 生产者填 `-1`），本 reader 与原 `fromFlattened` 一样统一返回 `-1` / `0` 而不误拒；但对于原 `fromFlattened` 可能放行、却不属于上述两类真实生产者的非规范输入，本 reader 直接拒绝且不回退到 JVM 重试。

## 所有权、预算和关闭

- `copyOf` 先校对 `payload` 预算，再拷贝 `flat` 与 `reply` 两个基本类型数组，并在副本上完成解析校验。调用方在构造期间不得并发修改输入；构造完成后，修改原数组不会影响 reader。
- `borrowImmutable` 保留两个数组的强引用，要求调用方在 `close()` 之前保持不可变。Java 强引用保活本身无法强制限制调用方写入，也不等同于原生 Memory lease；类内部不会暴露原始数组或可逃逸的元素视图。
- 所有读取、批量判断与 `close()` 由同一对象锁串行化。`close()` 幂等并清空两份数组引用，之后任何读取都会报错，但已返回的 `String` 和 `Decision` 位图继续独立有效；Java GC 或进程 RSS 回收时机不受保证。
- 可见 `payload` 预算公式为 `4 × flat.length + reply.length`：`copyOf` 记为 `owned`，`borrowImmutable` 记为 `borrowed`。该预算不含对象头、`Decision` 位图、原调用方持有的输入或 JVM 总体 RSS；采用复制构造时输入与副本会短暂共存，不可把单个 reader 的预算当成总峰值预算。
- `Decision` 另有 `2N + S` 字节的 `boolean` `payload` 预算（对象头另计），预算超限拒绝后 reader 仍可继续使用；调用 `decide` 期间不得并发修改传入的语义位图 `semanticBlocked`。

## 调用语义

读取字段存在性、有符号数值、内容区间与 UTF-8 文本与真实 `fromFlattened` 等价。`decide` 保留原有权重熔断规则：当分段没有任何正权重时放行权重过滤；存在正权重时，缺失 `weight` 的记录保留，有 `weight` 的记录按阈值过滤。语义拦截位图与权重过滤组合生效；VIP 改色仅作用于最终保留且存在 `colorful` 的条目，缺失 `kind` 的 source 保持保留。

`selectImmutable` 仅在原生路径不可用时才回退选择 JVM 生产者；若已选中的原生路径主动拒绝或返回非法 `flat`，直接返回 `null` 而不再调用 JVM 重试。Reader 不替代语义判断、文本规范化、wire 重写、重写后二次扫描、条数复检、`!changed` 原 `reply` 快路或宿主 `parseFrom`，`Decision` 位图可直接交给既有 rewriter。

## 适用范围与验证说明

验证证据见 [packed receipt](../artifacts/functional-next-20261030/packed/receipt.json)（使用 JDK `17` 与 Kotlin 编译器 `2.4.10` 在隔离目录独立编译两份真实源码作为 oracle，未构建外部项目）：

- **Java 合同测试（32 项）**：覆盖字段存在性与零值/32 位极值、空记录/超限、畸形计数与区间、预算拒绝、快照隔离、关闭后拒绝、跨线程读取与关闭、原生拒绝不回退。
- **真实 wire 差分（48 个 fixture × 6 种策略 = 288 项）**：涵盖 147 次成功二次扫描与 79 次规范重写拒绝，包含未知字段、重复字段、空内容与畸形 UTF-8；另已验证 `N` / `S` 均取 `65536` 的合法上限。
- **字节码与结构模型**：生产 JAR 仅含 4 个 class 文件，不引用 `ElemSignal`、`ArrayList` 或 `Integer` 装箱（`reader`、位图、异常及按需 `String` 分配仍存在，不宣称零分配）。`N+N′=5025` 为上述样本在原 `fromFlattened` 路径上的 `ElemSignal` 构造调用结构模型（`N′` 仅计入进入二次扫描的元素），并非真实 heap profiler 计数或时间/RSS/能耗收益。

本验证未在真实进程中运行 Native Rust/JNI 生产者、宿主 `parseFrom` 或整机回放；公开 CI 支持运行不依赖外部 oracle 的 `--java-only` 合同校验，不能替代本地真实 oracle 或设备端验证。
