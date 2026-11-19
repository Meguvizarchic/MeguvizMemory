# Android 托管域的可选压力接入

`ManagedMemoryDomain` 已提供显式选用的同域压力控制器。它使用同一 Memory 核心，不接管全局分配器，也不接管 Bitmap、HWUI、GPU 或其他库的 owner。本合同描述当前源码；宿主功能检查不证明 Android 真机、R8、性能或发布资格。[托管域合同](ANDROID_MANAGED_DOMAIN.md)说明基础生命周期；既有独立压力适配器见[Android 压力适配](ANDROID_PRESSURE.md)。

## 选用与事件

在工作线程创建域后，调用 `domain.attachPressure(moderateBytes, criticalBytes)`；两个值必须非负且 `criticalBytes <= moderateBytes`。每个域同时最多一个活动控制器。附着可能分配内存或创建线程；与预算切换或关闭竞争、资源不足时可能失败，应根据返回状态在工作线程处理，不应假定已附着。构造域时预算须为正，之后可通过 `setBudget(0)` 设置零软预算；v2 可在零预算附着，不会为了附着临时抬高预算。

`pressure.submit(level)` 接受 `NORMAL`、`MODERATE`、`CRITICAL`，正数是已排队的 ticket，负数是 native 状态码的相反数，例如 `-6` 表示 BUSY。受理是 QUEUED，并不表示预算已应用或 trim 完成；应在工作线程用 `eventResult(ticket)` 查询 `[state, status, ticket, applySequence]`，并按 QUEUED、APPLIED、FAILED、CANCELLED、EXPIRED 分别处理。null 表示结果不可用，不表示成功；无效票据也可能返回 null。事件不去重，每个受理事件分别计数。

队列使用固定容量，争用或队列满时 submit 返回 BUSY，包含 CRITICAL 在内的拒绝都不能当成已处理。事件在 worker 上按队列顺序处理；与并发的手动预算变更按实际应用顺序线性化，入队顺序不承诺优先于手动调用。等待结果或关闭应有调用方自己的期限；超时不会取消已受理事件，也不允许强制释放。

同一 binding 完成关闭后，可查询最近 128 个受理 ticket 的终态，包括 CANCELLED；更早的有效 ticket 显示 EXPIRED。关闭处理期间可能暂时返回 null；下一次附着尝试一旦进入 ATTACHING 阶段，旧 generation 的结果即不可查询，即使该次附着随后失败也不会恢复。`forgetClosed` 后同样不再提供旧结果。窗口有限，调用方需要在结果仍可用时及时读取，不得把 EXPIRED 或 null 当作 APPLIED。

## Android 回调组合

如需注册系统回调，显式创建 `ManagedPressureBackend(pressure)`，再将它交给 `new MemoryPressureAdapter(context, backend)`；仅调用 `attachPressure` 不会注册 Android 回调。adapter 负责注册与注销自己的 `ComponentCallbacks2`，其 `close()` 注销回调并请求关闭 backend，不关闭整个托管域。域先关闭时宿主仍须关闭 adapter，完成 framework callback 注销。并发 `close()` 若由其他线程正在执行，会抛出可重试的 `IllegalStateException`；同线程重入只返回给外层调用，不能视为关闭已接纳。`isClosed()` 只表示停止新事件，不证明注销和清理请求已经完成。

framework 回调只做事件投递，不同步执行 trim、free 或等待清理；Java monitor、JNI registry 和域准入仍可能使用短锁，无法保证严格无等待或硬实时。`MemoryPressureAdapter.forwardedEvents()` 在该 backend 下表示入队成功，不表示预算已应用；`ManagedPressureBackend.rejectedEvents()` 是饱和累计的拒绝数，`lastNotificationStatus()` 提供最近状态，零表示已排队。系统回调没有返回值，调用方应结合这些观测处理拒绝。`awaitClosed` 的正超时只能在工作线程调用。

Android 14（API 34）起，应用应围绕 `TRIM_MEMORY_UI_HIDDEN` 和 `TRIM_MEMORY_BACKGROUND` 处理系统通知。此 adapter 忽略 `UI_HIDDEN`，将 `BACKGROUND` 投递为 `MODERATE`。应用如果需要根据自己的生命周期释放可重建的内存，可以显式提交压力级别，也可以通过 `restoreBudget()` 显式恢复 NORMAL。[Android 应用内存管理](https://developer.android.com/topic/performance/memory/manage-app-memory)

## 预算与观测

附着时以域当前有效预算作为手动目标 `M`；附着期间 `domain.setBudget(bytes)` 与压力事件经同一仲裁点更新。NORMAL 的有效预算为 `M`，MODERATE 为 `min(M, moderateBytes)`，CRITICAL 为 `min(M, criticalBytes)`。每次应用只向核心提交一次最终预算，成功后才发布新状态；降低预算不撤销已有 owner、lease 或已准入 reservation。`setBudget` 同步完成本次预算应用，可能取锁，应安排在工作线程。已有待执行 trim 不因手动预算变更或 NORMAL 恢复而被撤销。

单独关闭压力 binding 不会自动恢复原手动目标，域保留关闭时的有效预算。再次附着以届时的核心预算作为新的 `M`；例如原目标 1000、关闭时有效预算 128，则重绑从 128 开始，若需要更高目标须由调用方显式 `setBudget`。域仍可能有已有 backing 超过新预算，预算不是 RSS 上限。

`pressure.snapshot()` 的字段依次是 `[state, appliedPressure, trimPending, lastTrimStatus, queueDepth, countersSaturated, manualBudget, effectiveBudget, applySequence, eventsAccepted, eventsApplied, eventsFailed, eventsCancelled, eventsBusy, trimAttempts, trimSuccesses, trimBusy, trimFailures, eventsCriticalBusy, resultsEvicted]`。null 表示不可用，`-1` 表示对应无符号值超出 Java long 范围；计数饱和标志置位后计数仅为下界。逻辑预算、backing、事件/trim 和进程 RSS/PSS 分别观察，trim 成功或预算降低都不证明系统低内存、物理回收或性能收益。

## 关闭、限制与隔离

`pressure.beginClose()` 请求异步关闭本 binding；`domain.beginClose()` 也会先停止压力事件准入，等待已入场调用，并完成 controller 的 begin-close、drain、销毁和 owner pin 归还，再关闭核心域。之后仍需等待其他 owner、lease、guard 和在途操作，才能完成核心清理。并发关闭只由一个清理执行者负责；资源准入失败可在工作线程重试，不会强制释放阻塞中的 provider 或 lease。

未知结果、销毁可能已消费或最终快照失败/不一致时保守隔离，保留必要记录及可保留的 pin，不重试可能已消费的指针，也不将 QUARANTINED 视为成功关闭。CLOSED 不是所有 Java/JNI 栈、系统线程或 GPU retirement 已退出的证明，不能据此卸载 DSO。

Java 绑定每个 controller 的事件队列为 32、结果窗口为 128，BUSY trim 重试间隔为 10 ms；底层 v2 API 限定队列 1–256、结果窗口 1–1024、重试间隔 1–60000 ms，每个压力 adapter runtime 最多 32 个 v2 worker。托管域清理另有每 JNI runtime 32 个 worker 的准入限制。它们是独立资源限制，并非核心 backing 预算或 OS 线程退出证明。

C v2 提供 `mgmem_pressure_create_v2`、`submit_v2`、`set_manual_budget_v2`、结果/快照和关闭接口；直接 C 调用者须经同一 controller 串行仲裁域预算，禁止在附着期间绕过它直接写核心预算，且须自行序列化 destroy 与其他原始调用。旧 v1 API 和 ABI 的同步通知语义保持不变；v1 初始 normal budget 仍须大于零。v1/v2 在同一 adapter runtime 共用单域附着互斥，不得同时附着。
