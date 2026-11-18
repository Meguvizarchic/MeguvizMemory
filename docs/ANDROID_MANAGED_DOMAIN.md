# 可选 Android 托管内存域

`ManagedMemoryDomain` 是现有共享 Memory 核心上的可选 Java owner/lease 包装，不提供原始地址。lease 另提供作用域受限的直接 ByteBuffer 视图 `withDirectBuffer(offset, length, action)`：缓冲别名 lease 原生区间（零拷贝，READ lease 为只读视图，字节序为 ByteBuffer 默认大端、需要时由回调内显式设置），回调期间 lease 监视器被持有故并发 close 不能中途释放，回调内自关会被拒绝，返回后缓冲被清零限长作尽力失效；保留引用在回调后或 lease 关闭后使用与保留 C 借用指针同等危险，属编程错误。异步交接不得使用该视图，应持有 lease 本身。`system(budgetBytes)` 和 `arena(budgetBytes, chunkBytes)` 分别使用默认 System 和 arena provider；不接管全局分配，也不支持通用外部内存导入。`attachPressure` 可显式启用同域压力控制；单独附着不会注册 Android 回调，需由宿主组合 `ManagedPressureBackend` 与 `MemoryPressureAdapter`。使用方法、预算仲裁及结果窗口见[同域压力合同](ANDROID_MANAGED_PRESSURE_CONTRACT.md)。

`allocate` 同步分配并对全部字节清零后交付；所有尺寸、对齐和范围必须合法。预算限制核心 backing 准入，不是 RSS 或进程总内存上限；降低预算不会撤销已有 owner 和 lease。元数据、Java 对象和清理线程不计入该预算。分配、清零、owner/lease 关闭可能耗时，安排在工作线程。应在将任务入队前取得 lease，任务完成后关闭 lease；owner 关闭不会使已取得的 lease 失效。不要依赖 GC 释放资源。

`beginClose()` 停止新操作准入并安排清理，不等待 owner 或 lease；已有准入操作可能在其返回后完成并交付结果。如已附着压力控制器，后台先封闭压力入场并销毁 controller、归还其 owner pin，再关闭核心域。关闭期间最后的 owner 和 lease 必须显式关闭；如果注册了系统回调，宿主还须关闭 adapter 以注销回调。线程或槽位不足时域保持 CLOSING，可以重试 `beginClose`；工作线程可自行限时轮询 `state`，超时不会强制销毁。`state`、`beginClose`、`forgetClosed` 和观测方法仍可能取短锁、创建线程或受调度影响，不承诺无锁或硬实时。重复 lease.close 在另一调用 RELEASING 时可能提前返回，必须看 `closeState` 判断完成。`forgetClosed` 仅删除已关闭域的观察令牌和保留的压力结果；并发重复调用可能提前返回，失败后可重试，不会重试核心 destroy。

当全部访问 raw domain 的核心 C 调用均已返回，owner、lease pin、注册取消/关闭 guard 均已清空后，后台 worker 才执行 collect、drain 和 destroy。借用适配器的 lifetime 回调返回后可能仍有适配器自身 registry/JNI 栈，wrapper guard 将 lease 关闭及取消结果纳入等待。Java 操作报告期内即使 native 已 CLOSED，`state` 保守报告 CLOSING，且 `forgetClosed` 拒绝；QUARANTINED 始终优先。`pendingManagedOperations()` 只是此 Java wrapper 正在报告的变更操作计数，不是 native 栈计数。

无法确认操作或 JNI 异常结果时保守隔离，保守保留无法确定状态的元数据与尚可保留的资源，不重复释放、不强制销毁；若 destroy 已消费域而其回执失败，原域可能已经不存在，不可假定内存一定仍保留；`QUARANTINED` 不是成功关闭。JNI 抛出的 RuntimeException 和 Error 原样传播，意外 checked 异常包装为 IllegalStateException 并保留 cause。Java 侧先记录 sticky 隔离，再尽力通知 native；即便通知失败或观察令牌已删除，也不能把错误视为消失。状态不保证严格单调：迟到的 Java 异常可使先前观察到的 CLOSED 变为 QUARANTINED；不承诺 Thread.stop 或进程崩溃的原子性。CLOSED 只说明已确认的核心清理与受管报告期结束，不证明其他 Java/JNI 栈或 detached worker 已退出；JNI 和核心库必须在进程内保持加载，不能据此卸载。

每个 JNI runtime 限制 1024 个域记录、65536 个 owner 记录、65536 个 lease pin、65536 个关闭 guard、32 个清理 worker；每个 Java 域还限制 65536 个同时报告操作。独立状态码区分这些槽位限制与核心 backing 预算不足。`runtimeCounts()` 各字段异时采样，记录可能处于创建、退休或隔离状态，不能解释为可用 handle、精确 live 数、RSS 或全进程清单；native 隔离计数可能漏掉 Java-only 未知状态。`javaQuarantineSignals()` 是每 classloader 饱和累计信号，不是资源数量。worker 数是准入槽位而非 OS 线程退出证明。

`snapshot()` 返回 `[managedState, coreState, budget, backing, reserved, peak, live, retiring, activeAllocations, activeLeases]`；managed 状态与核心快照分时采样，不能当作同一时刻的原子组合。activeAllocations 包括退休和可能在途的条目，不是当前 owner 数。null 表示原因未细分的不可用，不代表零；-1 表示超出 Java 有符号范围。观察不能据以自行 destroy 或判断 JNI 栈退出。

此合同及宿主回归不构成 Android 真机、R8、性能或发布资格证明。
