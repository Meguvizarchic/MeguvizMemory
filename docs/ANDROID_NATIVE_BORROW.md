# 在 Java 中借用原生内存

如果一个缓冲区已经由原生代码申请，可以把其中一段交给 Java 使用。原生侧注册借用，Java 用 `NativeMemoryLease.fromNativeRegistration(token)` 认领它。需要从 Java 申请新缓冲区时，使用 [ManagedMemoryDomain](ANDROID_MANAGED_DOMAIN.md)。

## 注册一段内存

原生侧包含 `<mgmem/android_borrow_jni.h>`，链接共享核心及 `libmgmem_pressure_jni.so`。通过 `mgmem_android_borrow_register_v1` 传入仍有 owner 的 allocation、字节范围、访问权限和域的保活回调。回调必须线程安全，确保域至少存活到最后一次原生借用释放调用返回。[Android 链接方式](ANDROID_QUICKSTART.md#应用含-c-或-c-代码)

注册前应初始化**整个可借用范围**，并传 `MGMEM_ANDROID_BORROW_INITIALIZED`。WRITE 借用也允许读取。原生 allocation 默认未初始化，这一标志由原持有者负责保证。通过其他原生别名或 DMA 访问同一地址时，仍需由来源负责同步。

token 在当前已加载的 JNI 运行库内有效，只能认领一次。Java 尚未认领便放弃交付时，用 `mgmem_android_borrow_cancel_v1` 取消。认领成功后由 Java 关闭 lease。回收不依赖 GC。owner 可先归还。已有 lease 在域进入关闭流程后仍能用完被借出的数据。

## 选择访问方式

| 接收方需要什么 | 方法 | 需要知道的限制 |
|---|---|---|
| 已有 Java 数组 | `read` / `write` | 两端都检查范围。`write` 每次最多 1 MiB，同一 JNI 运行库的并发写入暂存总额为 16 MiB。 |
| 数据要送到另一份 Memory lease | `source.copyTo(destination, srcOffset, dstOffset, count)` | 目标可写，每次最多 1 MiB。直接在两个范围间复制，重叠按 `memmove` 处理。 |
| 接口在调用期间同步消费 `ByteBuffer` | `withDirectBuffer(offset, length, action)` | 直接查看底层数据，最多 `Integer.MAX_VALUE` 字节。读借用交付只读视图。回调退出后，该视图和由它派生的视图都不能再使用。 |

`length()` 返回注册范围的长度，可大于单个 Java 数组。偏移均相对于当前 lease，单位为字节。数组 `write` 在暂存和 JNI 复制成功后才提交。拆成多次写入时，此前成功的部分会保留。`read` 遇到 JNI 异常时，目标数组可能已被部分修改。

`copyTo` 允许 READ 或 WRITE 源，也允许同一可写 lease 内自拷贝。已进入原生复制的调用可以在关闭开始后完成。若关闭先取得注册锁，复制会在写目标前被拒绝。JNI 调用本身抛出异常时无法据此判断字节是否已提交。Java 类和 JNI 应取自同一套制品。

直接视图默认大端。内层回调返回后，外层仍可访问自己的视图。回调内关闭当前 lease 会抛 `IllegalStateException`。若其他线程正在关闭它，在回调外等待该线程。需要跨线程持有数据的任务应在入队前取得自己的 lease，再在任务内部打开视图。若对方要求在回调后保留 `ByteBuffer`，就为它提供独立拥有的副本。

## 确认释放结果

`close()` 停止新访问并同步释放原生借用，可能在当前线程执行 provider 或原持有者的回调。应在允许阻塞的线程调用。并发重复 `close()` 可能提前返回，实际完成状态由 `closeState()` 给出。

| 状态 | 含义 |
|---|---|
| `CLOSE_OPEN` | 借用仍开放。 |
| `CLOSE_RELEASING` | 一个调用方正在释放，其他调用方应等待该次结果。 |
| `CLOSE_RELEASED` | 这一份借用的原生释放已成功返回。其他借用及外部 owner 有各自的寿命。 |
| `CLOSE_FAILED` | 首次释放报错或结果未知。用 `closeFailureStatus()` 取得原生状态，或 `CLOSE_FAILURE_UNKNOWN`。避免重试可能已消费的释放。 |

Java 数组中已经复制的数据在关闭后仍可使用。只要仍有注册、在途调用或隔离记录，就应保持 JNI 运行库和来源回调可用。[Java 图像读取示例](../consumers/java_image/README.md)
