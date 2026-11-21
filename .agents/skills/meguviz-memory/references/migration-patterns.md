# MeguvizMemory 迁移对照、生命周期与排障

## 1. 存量代码迁移对照范式

### 范式 A：同步临时 `std::vector` / `malloc` → `with_allocation`

```cpp
// 迁移前：裸分配，缺乏统一预算约束，漏掉异常释放分支易泄漏
auto *tmp = static_cast<uint8_t *>(std::malloc(bytes));
if (!tmp) return ERROR;
process(tmp, bytes);
std::free(tmp);

// 迁移后：受域预算约束，异常或提前返回均由 RAII 归还
mgmem_status status = domain.with_allocation(bytes, [](void *tmp, uint64_t len) {
    process(static_cast<uint8_t *>(tmp), len);
});
```

- **注意事项**：MeguvizMemory 返回的存储**未初始化**，写入前不得先读未赋值的字节；若 `bytes` 为小块且同批申请/同批结束，先用 `tools/run_adoption.py` 确认 `arena_recommended` 再选 `create_arena`。

### 范式 B：跨线程传递原始指针 → `acquire_borrow` 延寿

```cpp
// 迁移前：生产者释放后，后台任务读取造成 UAF
auto buf = std::make_shared<std::vector<uint8_t>>(bytes);
worker.post([buf] { consume(buf->data(), buf->size()); });

// 迁移后：主控方持有 owner，入队前为任务单独取得 READ borrow；owner 可随时归还
mgmem::owned_allocation owner;
mgmem_status s = domain.allocate(bytes, owner);
if (s == MGMEM_OK) {
    s = owner.with_write([](void *p, uint64_t n) { fill_data(p, n); });
}
if (s == MGMEM_OK) {
    mgmem::borrow lease;
    s = owner.acquire_borrow(MGMEM_READ, lease);
    if (s == MGMEM_OK) {
        worker.post([lease = std::move(lease)] { consume(lease.data(), lease.size_bytes()); });
    }
    // 即使此处立即释放 owner，底层存储仍由 lease 保活至任务结束
    (void)owner.release();
}
```

- **关键铁律**：**必须在把任务提交给执行器之前**调用 `acquire_borrow`；提交被拒或任务被取消时，随着任务对象析构，移动进去的 `mgmem::borrow` 会自动归还。

### 范式 C：已预加载缓冲区避免二次拷贝 → 外部导入（`import_external_checked`）

```cpp
mgmem_external_buffer_checked_v1 desc{};
desc.struct_size = sizeof(desc);
desc.abi_major = MGMEM_ABI_MAJOR;
desc.abi_minor = MGMEM_ABI_MINOR;
desc.data = external_ptr;
desc.capacity_bytes = external_bytes;
desc.alignment = 64;
desc.external_id = unique_nonzero_id; // 全 runtime 唯一，禁止与子区间复用同一 ID
desc.access = MGMEM_READ;
desc.context = external_owner_ctx;
desc.retain = &my_retain_noexcept;          // 失败原子，非 OK 时不增加引用
desc.release_checked = &my_release_noexcept; // 仅返回 MGMEM_OK 确认释放成功
desc.managing_runtime_origin = mgmem_runtime_origin_v1();

mgmem::owned_allocation imported;
mgmem_status s = domain.import_external_checked(desc, imported);
```

- **注意事项**：多个只读视图用同一 `imported` 的 `acquire_borrow(offset, len, MGMEM_READ, ...)` 表达，不要重复导入；不要把导入容量和原外部缓冲区的字节数相加当成物理总占用。

### 范式 D：Android 跨组件复制与 DirectBuffer 互操作

```java
// 迁移前：native -> byte[] -> 修改 -> byte[] -> native（多次堆数组分配与拷贝）
// 迁移后 1：两个 Memory Lease 之间直传（单次最多 1 MiB）
try (ManagedMemoryDomain.Lease src = srcSlot.borrowRead();
     ManagedMemoryDomain.Lease dst = dstSlot.borrowWrite()) {
    long offset = 0;
    while (offset < totalBytes) {
        int chunk = (int) Math.min(NativeMemoryLease.MAX_COPY_BYTES, totalBytes - offset);
        src.copyTo(dst, offset, offset, chunk);
        offset += chunk;
    }
}

// 迁移后 2：仅在同步回调内向 ByteBuffer 接口暴露直接视图
try (ManagedMemoryDomain.Lease writeLease = dstSlot.borrowWrite()) {
    writeLease.withDirectBuffer(buffer -> {
        buffer.order(ByteOrder.nativeOrder());
        codec.decodeInto(buffer); // buffer 及其 slice() 严禁逃出本 lambda
    });
}
```

### 范式 E：Android 托管域压力联动与关闭流程

1. 后台线程创建域并绑定压力控制器：`ManagedMemoryDomain.Pressure pressure = domain.attachPressure(moderate, critical);`。
2. 如需响应 Android `ComponentCallbacks2`：`MemoryPressureAdapter adapter = new MemoryPressureAdapter(context, new ManagedPressureBackend(pressure));`。
3. 退出清理顺序（**严禁在主线程做正超时等待**）：
   - 先调用 `adapter.close()` 注销系统回调并停止新事件；
   - 再停止业务侧新任务提交，调用 `domain.beginClose()`；
   - 等待在途 `Allocation` / `Lease` 全部 `close()`；
   - 在后台线程轮询 `domain.state()` 直到返回 `ManagedMemoryDomain.CLOSED`（若返回 `QUARANTINED`，通过 `failureStatus()` 记录错误，不得重试）；
   - 如需最后观测，先读 `domain.snapshot()`，再调用 `domain.forgetClosed()`。

## 2. 生命周期、线程与所有权铁律

| 维度 | 规则 | 违约后果 |
|---|---|---|
| **清理线程** | `mgmem_domain_collect_v1`、`mgmem_domain_destroy_v1`、最后一个 `mgmem::domain/owned_allocation/borrow` 析构、`NativeMemoryLease.close()`、`awaitClosed(timeout > 0)` 必须在可阻塞的工作线程执行 | 在 UI/完成回调线程阻塞，或在持有 `lease` 的线程等待自身关闭造成死锁 |
| **Checked 隔离态** | `import_external_checked` 或托管关闭返回 `MGMEM_PROVIDER_ERROR` / `QUARANTINED` 后，不得重试该句柄，必须保留域、回调代码与上下文直至进程退出或安全处理完成 | 重复释放外部资源引发 double-free 或未定义行为 |
| **Single Origin** | `mgmem_handle_v1` 和外部导入绑定到创建它的 `mgmem_runtime_origin_v1()`；同一进程不能把同一物理区域交给两份独立的 `libmgmem` / `mgmem.dll` 管理 | 跨运行库传递句柄返回 `MGMEM_STALE_HANDLE`；重复管理导致双重释放 |
| **DirectBuffer 作用域** | `withDirectBuffer` 回调返回时将 `limit` 置 0；不得保存 `buffer` 或其 `slice()`/`duplicate()` 到回调外，也不得在回调内调用 `lease.close()` | 回调内 `close()` 抛 `IllegalStateException`；回调外访问释放后内存引发崩溃 |
| **读写互斥** | `MGMEM_WRITE` 同一时刻独占所属 `allocation`；`MGMEM_READ` 允许多个读者并存 | 并发申请写借用或读期间申请写借用返回 `MGMEM_BUSY` |

## 3. 高频错误与排查表

| 现象 / 状态码 | 常见原因 | 排查与修正方案 |
|---|---|---|
| `MGMEM_OUT_OF_BUDGET` (4) | 新申请或外部导入加上 `reserved_bytes` 超出 `budget_bytes` | 先释放不再使用的 `owner`，或检查扩容时是否预留了新旧共存峰值；Arena 有空闲保留空间也受软预算限制 |
| `MGMEM_BUSY` (6) | 1. 同一 `allocation` 存在冲突借用；<br>2. `reset`/`trim`/`collect` 时仍有存活 `owner`/`lease` 或回调；<br>3. `pressure` 队列满或正在附着 | 检查是否有未归还的异步 `lease`；`collect` 前先确认所有借用释放完毕；压力提交仅对未受理的 `BUSY` 做有界退避重试 |
| `MGMEM_STALE_HANDLE` (5) | 使用了已释放的句柄，或把句柄传给了另一份独立加载的 Memory 运行库 | 核对 APK/进程内是否打包或加载了多份 `libmgmem.so` / `mgmem.dll`；确保全进程共用同一份共享库 |
| `MGMEM_UNSUPPORTED` (8) | 向 System 专属接口传入 Arena 域，或调用 `import_external_checked` / `import_external_arena*` 时运行库缺少对应能力位 | 检查 `mgmem_capabilities_v1()` 是否包含 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE` / `MGMEM_CAP_ARENA_EXTERNAL_IMPORT` |
| `ManagedException` 状态 `256..260` | 达到 JNI 运行库注册配额（1,024 域 / 65,536 分配 / 65,536 租约 / 32 清理 worker / 65,536 保护记录） | 检查是否存在未关闭的 `Domain`/`Allocation`/`Lease` 或忘记 `forgetClosed()`；降低并发清理数或分批等待 |
| APK 打包报错重复 `libmgmem.so` | Prefab、独立 NDK SDK 与 `jniLibs` 多处放入了核心库 | 移除重复拷贝，统一只通过 AAR Prefab（`mgmem-android::mgmem`）或单一本地 SDK 接入，确保版本同源 |
| `shadow_feedback` 报告 `DRIFT` | 真实运行快照的 `provider_allocation_count` 或 `peak_backing_bytes` 与影子预测偏差超 10% | 重新采集真实批次边界（`{"op":"reset"}`）与释放事件，重算 `chunk` 尺寸后再决定是否保留 Arena |

## 4. 安全回退方案

1. **只切新域，不迁移或强杀旧域**：回退开关仅改变后续新建任务的域创建或分配入口（例如从 Arena 切回 System，或切回原分配器）。
2. **自然收尾**：存量正在运行的域和 `lease` 按原释放与 `begin_close` → `collect` → `drain` → `destroy`（或 Android `beginClose()` → `CLOSED` → `forgetClosed()`）流程结束。
3. **保留保活与回调**：若回退前已有外部导入或隔离域，继续保持其 `retain`/`release` 回调代码、上下文及 Memory 动态库处于加载状态，直到所有在途借用归还完毕。
