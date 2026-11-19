# Android 托管内存域

`ManagedMemoryDomain` 适合需要显式预算、从 Java 或 Kotlin 管理原生 CPU 缓冲区的组件。组件持有域，调用方持有每次分配得到的 `Allocation`，任务在访问字节期间持有 `Lease`。[Android 接入示例](ANDROID_QUICKSTART.md#读写第一块缓冲区)

## 申请和访问

- `system(budgetBytes)` 适合各块独立释放的缓冲区。`arena(budgetBytes, chunkBytes)` 适合一批一起使用的临时数据，可为一批工作创建一个托管域，在批末关闭。需要显式复位同一个 Arena、跨批复用底层块时，使用原生接口。[原生 Arena 的复用](CPP_CONVENIENCE.md#域的结束与复用)
- `allocate(bytes, alignment)` 同步申请并清零整个范围。预算针对本域底层存储。降低预算只影响后续申请，已有的 owner 和 lease 可以继续使用。大块清零和最后一次释放应由工作线程承担。
- `borrowRead(offset, bytes)` 可与同一 allocation 的其他读借用共存，`borrowWrite` 需要独占。任务跨线程时，先取得 lease，再提交任务。若提交被拒绝、任务取消或抛出异常，负责该任务的一方仍需关闭 lease。关闭 allocation 后，已取得的 lease 可以继续访问。资源需要显式归还。

已有 `byte[]` 时用 `read` 或 `write`，两端都是 lease 时用 `copyTo`。只在同步回调里接受 `ByteBuffer` 的接口适合用 `withDirectBuffer`。READ 借用得到只读视图，视图初始为大端。读写原生数值时按数据格式显式选择字节序。视图长度为 1 到 `Integer.MAX_VALUE` 字节，并须落在 lease 内。[传输上限和关闭状态](ANDROID_NATIVE_BORROW.md)

从当前源码构建时可用 `allocation.borrowRead()` 或 `allocation.borrowWrite()` 借用整个 allocation，再用 `lease.withDirectBuffer(action)` 查看整个 lease。大于 `Integer.MAX_VALUE` 的 lease 可用带偏移和长度的重载分段查看。

视图及其 `slice()`、`duplicate()` 都只能在本次回调内使用。正常或异常退出时，库会把交给回调的视图的 `limit` 设为零。调用方创建的切片和副本视图仍受同一使用期限约束。同一线程可嵌套访问同一 lease，内层返回后外层视图仍可用。在回调内关闭这份 lease 会抛出 `IllegalStateException`。其他线程的释放可能等待回调结束，所以不要在回调里等待负责关闭它的线程。

## 关闭内存域

`beginClose()` 和 `close()` 都会停止新操作并安排后台清理。已准入的操作可能稍后才交付结果。域的持有者应保留域引用，直到关闭得到确认。应用若注册过系统压力回调，还要关闭对应的 adapter 以注销回调。[带压力回调的完整示例](../consumers/android_managed/README.md)

| `state()` | 调用方的处理 |
|---|---|
| `OPEN` | 可以申请及借用。准备退出时，先停止提交新任务，再调用 `beginClose()`。 |
| `CLOSING` | 让已受理的任务归还 owner 和 lease，由后台收集并销毁域。使用有时间上限的后台等待，超时后保留引用，稍后再查。若 `beginClose()` 因清理线程名额不足失败，可在稍后重试该调用。 |
| `CLOSED` | 此域的核心清理已得到确认。若需最后一份快照，先读取，再调 `forgetClosed()` 归还观察令牌及保留的压力结果。 |
| `QUARANTINED` | 有操作的结果无法确认。通过 `failureStatus()` 记录错误，停止使用该域。可能已消费的释放不能重试。 |

多个线程同时关闭一份 lease 时，重复调用可能在第一次释放结束前返回。用 `closeState()` 区分 `CLOSE_RELEASING`、`CLOSE_RELEASED` 和 `CLOSE_FAILED`。失败时再读 `closeFailureStatus()`。如果托管关闭尚未进入实际释放便因名额不足被拒，原调用抛错，清理收尾后状态恢复到 `CLOSE_OPEN`，这时可重试。

后台清理会等已准入的原生调用、借用和关闭操作结束。Java 仍在报告操作结果时，`state()` 会暂报 `CLOSING`，`forgetClosed()` 也可能暂时被拒。并发的重复 `forgetClosed()` 可先返回。归还令牌失败后可以重试这一步。晚到的 JNI 错误可能使已观察为 `CLOSED` 的域转入隔离。核心和 JNI 库应在进程内持续加载。

## 预算、压力与诊断

`attachPressure(moderateBytes, criticalBytes)` 为同一域绑定压力控制器。需要 Android 系统回调时，组合 `ManagedPressureBackend` 和 `MemoryPressureAdapter`。域关闭时，后台先停止控制器、释放其持有的引用，再关闭核心域。[压力控制器与事件结果](ANDROID_MANAGED_PRESSURE_CONTRACT.md)

`snapshot()` 返回以下字段，单位为字节的数值包括 `budget` 到 `retiring`。其中 `reserved` 是尚在进行的分配预留，`peak` 是底层容量加预留的历史峰值。

```text
[managedState, coreState, budget, backing, reserved, peak, live, retiring, activeAllocations, activeLeases]
```

托管状态和核心状态分时采样。`activeAllocations` 也可能包含 owner 已归还但仍有 lease 的记录。`null` 表示快照不可用，`-1` 表示字段超出 Java 有符号整数范围。遗忘令牌后读取可能抛异常。元数据、Java 对象、线程栈和进程 RSS 需单独观测。

每份 JNI 运行库最多登记 1,024 个域、65,536 个 allocation、65,536 个 lease 和 65,536 个关闭保护记录，同时最多准入 32 个清理 worker。一个 Java 域同时报告的操作上限为 65,536。`ManagedException.status()` 能区分登记名额耗尽和底层存储预算不足。`pendingManagedOperations()` 仅统计当前 Java 域正在报告的操作。`runtimeCounts()` 分项采样，包含创建中、回收中和隔离记录。`javaQuarantineSignals()` 是当前类加载器的累计隔离信号。
