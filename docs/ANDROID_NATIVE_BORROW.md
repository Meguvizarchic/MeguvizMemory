# 在 Java 中借用原生内存

当缓冲区已经由原生 C/C++ 代码申请时，可把其中一段受控地借给 Java 使用：原生侧调用注册函数取得 token，Java 侧通过 `NativeMemoryLease.fromNativeRegistration(token)` 认领。如果缓冲区本身就由 Java 申请管理，请直接使用 [ManagedMemoryDomain](ANDROID_MANAGED_DOMAIN.md)。

## 注册一段内存

原生侧包含 `<mgmem/android_borrow_jni.h>`，并链接共享核心库与 `libmgmem_pressure_jni.so`。调用 `mgmem_android_borrow_register_v1` 时传入有效的 `owner`、偏移与字节长度、访问权限（`MGMEM_READ` / `MGMEM_WRITE`）以及域的保活回调表 `mgmem_domain_lifetime_v1`。回调必须保证线程安全且不抛出异常，并确保域在最后一次原生借用释放完成前保持存活。[Android 链接方式](ANDROID_QUICKSTART.md#应用含-c-或-c-代码)

注册前必须先**初始化整个可借用范围**，并传入 `MGMEM_ANDROID_BORROW_INITIALIZED`。因为原生 `owner` 默认未初始化，而 `WRITE` 借用同样允许读取数据，该标志由原生持有方负责保证。若通过其他原生别名或 DMA 并发访问同一地址，仍需来源方自行同步。

注册返回的 token 仅在当前已加载的 JNI 运行库内有效，且只能认领一次。如果 Java 尚未认领前决定取消交付，可调用 `mgmem_android_borrow_cancel_v1` 撤销；一旦认领成功，就改由 Java 侧调用 `close()` 归还。归还依赖显式关闭而不依赖 GC；原生 `owner` 可提前归还，已取得的 `Lease` 在域进入关闭流程后仍可读完既定数据。

## 选择访问方式

| 接收方需要什么 | 方法 | 限制与说明 |
|---|---|---|
| 已有 Java `byte[]` 数组 | `read` / `write` | 两端均校验范围。`write` 单次上限为 `1 MiB`，同一 JNI 运行库并发写入暂存上限为 `16 MiB`。 |
| 数据送到另一份 Memory `Lease` | `source.copyTo(destination, srcOffset, dstOffset, count)` | 目标须为可写借用，单次上限 `1 MiB`；直接在两个原生范围间复制，重叠按 `memmove` 处理。 |
| 接口在同步回调内消费 `ByteBuffer` | `withDirectBuffer(action)` / `withDirectBuffer(offset, length, action)` | 直接映射底层范围，单视图上限 `Integer.MAX_VALUE` 字节；读借用返回只读视图，回调返回后视图及其派生切片均失效。 |

`length()` 返回整个注册范围的长度（可超过单个 Java 数组上限），偏移量均以当前 `Lease` 为起点、单位为字节。数组 `write` 仅在暂存分配与 JNI 复制全部成功后才提交（拆分成多次写入时，此前成功的分段会保留）；`read` 若遇到 JNI 异常，目标数组可能已被部分改写。

`copyTo` 支持 `READ` 或 `WRITE` 作为源，也允许在同一个可写 `Lease` 内部自拷贝。已开始的传输可在关闭发起后继续完成；若关闭先取得注册锁，传输会在写入目标前被拒绝。若 JNI 调用抛出异常，无法据此确认字节是否已经写入目标。Java 类与原生 JNI 库必须来自同一批次构建。

直接视图的默认字节序为大端（big-endian）。同一线程嵌套回调时，内层返回后外层视图依然有效；由于回调期间持有当前 `Lease` 的对象锁，回调内调用 `close()` 会抛出 `IllegalStateException`。跨 `Lease` 的 `copyTo` 与线程等待不要放在直接视图回调内执行，同时应协调已提交的传输和直接访问。跨线程使用数据时，应在提交任务前取得各自独立的 `Lease`，并在任务内部打开视图；若第三方接口要求在回调结束后长期保留 `ByteBuffer`，请为其提供独立拥有的副本。

## 确认释放结果

`close()` 会停止后续访问并同步释放原生借用，可能在当前线程直接触发 provider 或来源持有者的释放回调，因此应在允许阻塞的后台线程调用。多线程并发调用 `close()` 时，重复调用可能在第一次释放结束前返回，实际结果通过 `closeState()` 确认：

| 状态 | 含义 |
|---|---|
| `CLOSE_OPEN` | 借用仍开放。 |
| `CLOSE_RELEASING` | 正在执行释放，其他并发调用方应等待该次结果。 |
| `CLOSE_RELEASED` | 该份借用的原生释放已成功返回；其他借用及外部 `owner` 有各自独立的生命周期。 |
| `CLOSE_FAILED` | 首次释放报错或结果未知。通过 `closeFailureStatus()` 获取状态码（或 `CLOSE_FAILURE_UNKNOWN`），禁止重试可能已被消费的释放。 |

已经复制到 Java 数组中的数据在 `Lease` 关闭后仍可继续使用；只要进程内仍有未关闭注册、进行中的调用或隔离记录，就必须保持 JNI 与核心运行库加载有效，不可提前卸载。[Java 图像读取示例](../consumers/java_image/README.md)
