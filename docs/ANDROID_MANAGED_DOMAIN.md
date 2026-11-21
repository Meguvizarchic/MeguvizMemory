# Android 托管内存域

`ManagedMemoryDomain` 适合需要在 Java 或 Kotlin 中以显式预算管理原生 CPU 缓冲区的组件。组件生命周期持有域对象，调用方持有每次分配得到的 `Allocation`，任务在访问字节期间持有 `Lease`。[Android 接入示例](ANDROID_QUICKSTART.md#读写第一块缓冲区)

## 申请和访问

- `system(budgetBytes)` 适合各块缓冲区独立释放的场景。`arena(budgetBytes, chunkBytes)` 适合整批一起申请并一同结束的临时数据，可为单批工作创建一个托管域并在批末关闭。需要显式复位同一个 Arena、跨批复用底层块时，请使用原生 C/C++ 接口。[原生 Arena 的复用](CPP_CONVENIENCE.md#域的结束与复用)
- `allocate(bytes, alignment)` 会同步申请并**清零**整段范围。预算只统计本域的底层存储；降低预算仅影响后续新申请，已有的 `Allocation` 和 `Lease` 仍可继续使用。大块清零和最后一次释放应安排在后台工作线程。
- `borrowRead()` / `borrowRead(offset, bytes)` 可与同一分配上的其他读借用共存，`borrowWrite()` / `borrowWrite(offset, bytes)` 则要求独占。任务跨线程时，应在提交前先取得 `Lease` 再移交给任务；无论是提交被拒、任务取消还是异常退出，负责该任务的一方都必须关闭 `Lease`。关闭 `Allocation` 后，已取得的 `Lease` 仍可正常访问直到自身关闭。

已有 `byte[]` 时使用 `read` / `write`，两端都已是 `Lease` 时使用 `copyTo`。只接受 `ByteBuffer` 的同步接口适合使用 `withDirectBuffer(action)` 或分段重载 `withDirectBuffer(offset, length, action)`。READ 借用交付只读视图，所有直接视图的初始字节序均为大端（big-endian），读写原生数值时须按格式显式设置字节序。单次直接视图长度范围为 `1` 到 `Integer.MAX_VALUE` 字节；大于 `Integer.MAX_VALUE` 的借用请用偏移与长度重载分段查看。[传输上限和关闭状态](ANDROID_NATIVE_BORROW.md)

直接视图及其 `slice()`、`duplicate()` 视图都只能在本次回调内使用；回调正常或异常返回时，库会将传入视图的 `limit` 设为 `0`。同一线程允许嵌套访问同一 `Lease`，内层返回后外层视图仍可使用；在回调内部调用该 `Lease` 的 `close()` 会抛出 `IllegalStateException`。由于其他线程调用 `close()` 会等待回调结束，不要在回调内部等待负责关闭该借用的线程。

## 关闭内存域

`beginClose()` 和 `close()` 都会立即拒绝新的申请与借用，并调度后台清理线程；已受理的操作仍可能稍后交付结果。域的持有者应保留对象引用直到确认关闭完成。若注册过系统压力回调，也须同步关闭对应的 `MemoryPressureAdapter` 以注销回调。[带压力回调的完整示例](../consumers/android_managed/README.md)

| `state()` | 调用方的处理 |
|---|---|
| `OPEN` | 可正常申请与借用。准备退出时，先停止提交新任务，再调用 `beginClose()`。 |
| `CLOSING` | 让已受理的任务归还全部 `Allocation` 和 `Lease`，由后台收集并销毁域。应在工作线程使用有界轮询或等待；超时后保留引用稍后再查。若 `beginClose()` 因清理线程配额不足失败，可稍后重试。 |
| `CLOSED` | 域的核心清理已确认完成。若需最后观测，先读取 `snapshot()`，再调用 `forgetClosed()` 释放观察令牌与保留的压力结果。 |
| `QUARANTINED` | 某次操作结果无法确认。通过 `failureStatus()` 记录错误码并停止使用该域，禁止重试可能已消费的释放操作。 |

多个线程同时关闭同一份 `Lease` 时，后续重复调用可能在首次释放尚未结束前返回，此时可通过 `closeState()` 区分 `CLOSE_RELEASING`、`CLOSE_RELEASED` 与 `CLOSE_FAILED`，失败时再用 `closeFailureStatus()` 读取状态。若在尚未进入底层释放前因名额不足被拒，该次调用抛出异常并把状态恢复为 `CLOSE_OPEN`，之后可重试关闭。

后台清理会等待已受理的原生调用、借用与关闭操作退出。Java 仍在报告操作结果期间，`state()` 会暂时保持 `CLOSING`，`forgetClosed()` 也会被暂时拒绝；重复并发调用 `forgetClosed()` 可能先返回，归还令牌失败后可重试这一步。晚到的 JNI 异常仍可能使已观察到 `CLOSED` 的域转入 `QUARANTINED`；核心库与 JNI 库应在进程生命周期内持续加载。

## 预算、压力与诊断

`attachPressure(moderateBytes, criticalBytes)` 为当前域绑定同域压力控制器；如需接入 Android 系统回调，须另行组合 `ManagedPressureBackend` 与 `MemoryPressureAdapter`。域关闭时，后台线程会先停止压力控制器并释放其持有引用，再关闭核心域。[压力控制器与事件结果](ANDROID_MANAGED_PRESSURE_CONTRACT.md)

`snapshot()` 返回 10 项指标（其中 `budget` 到 `retiring` 单位为字节，`reserved` 为进行中的分配预留，`peak` 为底层容量加预留的历史峰值）：

```text
[managedState, coreState, budget, backing, reserved, peak, live, retiring, activeAllocations, activeLeases]
```

托管状态和核心状态分时采样。`activeAllocations` 包含 `Allocation` 已关闭但仍有 `Lease` 存活的条目。返回 `null` 表示快照暂时不可用，字段为 `-1` 表示数值超出 Java `long` 有符号范围；调用 `forgetClosed()` 之后再读快照可能抛异常。元数据、Java 对象、线程栈和进程 RSS 需另外观测。

每份 JNI 运行库最多可登记 `1,024` 个域、`65,536` 个分配、`65,536` 个租约和 `65,536` 个关闭保护记录，最多同时运行 `32` 个清理 worker；单个 Java 域同时报告的操作上限为 `65,536`。通过 `ManagedException.status()`（状态码 `256`–`260`）可区分登记配额耗尽与底层存储预算不足。`pendingManagedOperations()` 仅统计当前 Java 域仍在报告中的操作；`runtimeCounts()` 包含创建中、回收中和隔离态条目；`javaQuarantineSignals()` 为当前类加载器累计的保守隔离信号数。
