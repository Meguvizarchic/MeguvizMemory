# Android 受控原生字节借用

这是可选 JNI 适配层。可信原生代码通过 `mgmem_android_borrow_register_v1` 把已有 Memory allocation 的一段 READ 或 WRITE lease 注册为进程内 token；Java 用 `NativeMemoryLease.fromNativeRegistration` 单次认领，然后通过 `read` / `write` 在已有 `byte[]` 与受控范围之间复制。该接口不返回原生指针、`DirectByteBuffer` 或其他可绕过关闭检查的引用。`NativeMemoryLease` 本身不提供 allocation 工厂；自有 System/Arena 域可使用独立的 [ManagedMemoryDomain](ANDROID_MANAGED_DOMAIN.md)，两者均不接管进程 allocator。

## 原生依赖与归属

声明位于安装后的 `include/mgmem/android_borrow_jni.h`，实现由 Android JNI 组合库 `libmgmem_pressure_jni.so` 提供，而非 `libmgmem.so`。同一进程的原生消费者须链接同一个已安装的共享 Memory core 和此 JNI 库，不能把 Memory 静态嵌入成第二套 runtime。该头文件依赖现有 `mgmem/pressure.h` 中的 `mgmem_domain_lifetime_v1` 合同；没有新增第三方生产依赖。

注册要求有效 domain、仍有 owner 的 allocation handle、范围和原 domain owner 的线程安全保活回调。调用方须一直保持这些参数有效直到注册调用返回；已取得的 lease 之后可在 domain 进入 CLOSING、allocation owner 已释放的情况下继续访问。domain owner 的 retain/release 负责保证 domain 在最后一次 `mgmem_borrow_release_v1` 完整返回前不会被销毁。适配器不关闭或销毁外部 domain。

**必须先初始化整个可借用范围**，并传 `MGMEM_ANDROID_BORROW_INITIALIZED`。Memory 的原生 allocation 默认未初始化；此标志是 origin owner 的明确保证，不是适配器能验证的事实。WRITE lease 同时允许读取，也必须先初始化。Memory 管理范围之外的别名、DMA、线程同步和外部 owner 行为仍由原 owner 负责；不能把 Memory lease 当作对其他别名的硬件访问保护。

token 是只在当前已加载 JNI runtime 内有效、单调且不复用的标识，不是地址，也不是访问控制或可持久化的凭据。注册成功后只能被一个 Java wrapper 认领；认领前若绑定失败或不再需要，由原生调用方调用 `mgmem_android_borrow_cancel_v1`。Java 已认领的 token 不能再由 cancel 关闭。未认领 token 和忘记关闭的 Java lease 会保留 native lease 与 owner；不依赖 GC/Cleaner 自动恢复，也不把元数据计入 domain backing 预算。

## 访问和关闭

`length` 是注册范围长度，可大于单个 Java 数组。每次读写都分别检查 lease 范围、Java 数组范围、非负值及权限；native 侧也会重新检查。READ lease 不支持写入，WRITE lease 是 core 的独占写借用。Java 方法同步串行化同一 wrapper 的访问和 close；native 层也会等待已入场访问完成后再释放 lease。数组传输期间不保存 Java 数组或数组指针，JNI 抛出的异常保留给调用方。读入调用方 `byte[]` 时，如果 JNI 发生异常，目标数组可能已被部分修改，不能假定其保持原值。

每次 `write` 上限为 1 MiB；该 JNI runtime 内所有注册共享最多 16 MiB 的并发写入暂存 payload 配额。如果多个类加载器或重复装载的 DSO 导致独立的 JNI runtime 副本，各副本不共享这项配额，因此它不是强制的进程总上限。申请配额、分配暂存或 JNI 从 Java 数组复制失败时，native backing 不变；只有复制成功才提交。暂存 payload 不计入 domain 预算，分配器开销和其他进程内内存也不包含在该配额中；额外复制、分配及并发额度拒绝是使用成本。更大写入由调用方显式拆为多次独立提交：前一笔成功后，后一笔失败不会回滚前一笔。该保证针对可恢复错误，不保证崩溃原子，也不保证其他未经同步的原生别名看不到提交过程。

`copyTo` 是新增的 Java/JNI 配对能力；使用包含它的同批 JAR/AAR 与 JNI 动态库。旧 JNI 库没有 `nativeTransfer`，仅有相同 SDK 版本号不表示新入口可用；混配时调用会链接失败，不能据此重试为重复提交。原有 `read`/`write` 的调用合同不变。

两端已经是 Memory lease 时，使用 `source.copyTo(destination, sourceOffset, destinationOffset, count)`，`ManagedMemoryDomain.Lease` 同样提供该入口。目标必须为 WRITE lease，每次最多 `NativeMemoryLease.MAX_COPY_BYTES`（1 MiB），源可为 READ 或 WRITE；重叠和自拷贝采用 `memmove` 语义。它在一次普通 JNI 调用中直接复制，不创建 Java 中转数组或 native 写入暂存；这是一次字节复制，不是零拷贝。域 backing 计费不变，目标须事先分配和初始化。双方 Java token 在各自短锁下采样，之后由 native 按 token 顺序锁定两个注册；Java 不同时持有两个对象锁。已入场传输可在 `close` 开始后完成；若原生关闭先获锁，传输失败且不写目标。可恢复的参数、权限或关闭拒绝均在复制前完成；JNI 返回异常仍不能推断复制是否发生，也不保证未经同步的外部别名原子观察。不同调用独立提交。

首次 `close` 在对象锁内停止新访问，释放该锁后同步执行原生释放；它可能在调用线程执行原 provider 和 domain-owner release 回调，应由应用选择合适的清理线程，不能默认在 UI 线程调用。并发或回调中的重复 `close` 是幂等的，但可在首次释放尚未完成时先返回，不表示清理完成；只有首次调用者获得原生释放错误。关闭后不再接受访问；现有 Java byte[] 中的副本不受影响。原生释放报告错误时适配器保守隔离该注册；若 JNI 没有返回，原生状态可能未知。两种情况都不能假定 lease 或回调未被消费，也不能重试或声称资源归零。若 foreign 回调违背 no-throw 合同，则其引用状态可能未知；适配器不能修复违约或保证强制回收。JNI DSO 在注册、调用或隔离项仍存在时不支持卸载。

`closeState()` 只作不等待清理的状态读取：`CLOSE_OPEN`、`CLOSE_RELEASING`、`CLOSE_RELEASED`、`CLOSE_FAILED`。`CLOSE_RELEASED` 表示本次注册的 `nativeClose` 成功返回，不表示其他 lease、外部 owner、整个 domain 或系统驻留已归零。`CLOSE_FAILED` 表示首次调用失败或结果未知：JNI 异常可能在原生句柄已消费后才到达，不能据此重试，也不能断定资源是否回收。读取到失败后 `closeFailureStatus()` 才提供已知 native 状态；若 JNI 未返回状态则为 `CLOSE_FAILURE_UNKNOWN`。实现先发布错误码、再发布 volatile 失败状态；首次 `close` 仍向调用者抛出原始异常或报告 native 错误。

状态读取不等待释放回调，但不承诺硬实时或免受 VM/系统暂停影响。进程、线程被终止或永久冻结时，`CLOSE_RELEASING` 可能长期保持；不支持通过异步 `Thread.stop` 或运行中卸载 DSO 来强制恢复。此状态不会替代 owner/domain 原有的收尾与观测合同。

## 当前验证边界

独立宿主测试 `tests/borrow_host` 以真实 core 加 fake JNI 覆盖初始化标志、重复认领、权限/边界、owner 先释放、domain 关闭后访问、访问与关闭竞争、pending JNI 异常及失败回调隔离；另一项使用真实宿主 JVM、真实 JNI 和 core 进行字节读写、关闭与域清理。该测试组合的宿主 JVM fixture 将 core 编入测试 DLL，仅用于单进程测试，不是 Android 发布库链接方式。

独立 [Java 图像消费者](../consumers/java_image/README.md)已迁移到 ManagedMemoryDomain，并在宿主通过安装后的共享核心和单独构建的 host JNI 使用受控借用；它不是 Android 生产消费者。此前 arm64 R8 构建覆盖托管域加入前的版本，见[历史构建证据](../artifacts/continuation-next-20261101/android-build/r8_build.json)；后续托管域和合成压力在 API33 arm64/4KB 已完成限定的 [R8 真机短测](../artifacts/managed-probe-20261101/device-short-pass-20261101.json)。该短测早于后续压力关闭准入修复；修复版 ART、长期稳定性、真实系统压力和真实 Android 消费者接入仍须分别验证。BIL 未接入，也无性能或零拷贝收益声明。

## JNI 传输设计依据

[Android JNI 指南](https://developer.android.com/ndk/guides/jni-tips)优先建议减少跨层数据封送及调用频率，单纯数组复制应使用 Region API。原有 `read`/`write` 已采用这些 API，本次新增 lease 间直接传输只减少两端均为既有 lease 时的中转成本。[ART/libcore 的 Memory 接口](https://android.googlesource.com/platform/libcore/+/master/luni/src/main/java/libcore/io/Memory.java)也提供批量 memmove 作为 NIO 的实现机制；本库不调用这些隐藏接口。现有路径有 mutex 和生命周期管理，继续使用普通 JNI；[CriticalNative 合同](https://developer.android.com/reference/dalvik/annotation/optimization/CriticalNative)不适合可能等待的锁或长期操作。结构性减少不能代替同语义配对性能测量；此入口仍需独立 Android/R8/设备验证。
