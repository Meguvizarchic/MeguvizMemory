# Memory 与 Compute 缓冲传输

`<mgmem/compute_transfer.h>` 提供 C 的 `mgmem_upload(&transfer)`、`mgmem_download(&transfer)`，C++ 可用 `mgmem::compute::upload(transfer)` 和 `mgmem::compute::download(transfer)`。链接 `MeguvizMemoryComputeHostBridge::host_bridge`。

```c
mgmem_transfer transfer = {0};
transfer.memory = memory_domain;
transfer.allocation = allocation;
transfer.offset = 0;
transfer.bytes = byte_count;
transfer.compute = compute_domain;
transfer.session = session;
transfer.device = gpu_write_lease;
transfer.request_id = request_id;
transfer.completion = on_completion;
transfer.context = completion_context;
transfer.on_error = on_memory_error;
transfer.error_context = error_context;
mgmem_transfer_result result = mgmem_upload(&transfer);
if (result.status == MGMEM_OK && result.origin_status == MGC_ACCEPTED) {
    /* 检查异步完成状态，随后释放 result.ticket。 */
    mgc_ticket_release(result.ticket);
}
```

上传需要目标 WRITE lease；下载需要来源 READ lease。传输复用既有 Compute `COPY`，在 Vulkan 路径会进行中转分配和 memcpy，再上传或下载；不是 GPU 内存零拷贝，也不保证性能提升。Vulkan 路径要求长度和两侧 lease 偏移为 4 字节倍数，GPU 偏移还须满足设备对齐要求。AHardwareBuffer 不支持该 `COPY` 路径；异步任务可能在提交已接受后因对齐、设备丢失或不支持的资源类型而失败，应检查 `MgcCompletion` 的终态和状态。

调用者的 Compute lease 在成功和失败时均仍归调用者持有；桥只为提交保留并转移额外引用。Memory 缓冲的读或写借用一直保持到提交引用归还，即 GPU 工作已退休、主机最终处理和完成回调结束之后。回调可能在函数返回前执行；其上下文及 Memory domain、错误回调上下文需要保持存活，回调不得抛异常。不要在任务退休前访问提交的 lease。`mgc_ticket_query` 显示完成并不表示回调已返回；需要等待 `mgc_session_drain` 才能确认提交引用已经归还。不要在该 session 的完成回调内调用 drain，Compute 会返回 BUSY。如果失败返回没有 ticket，Memory 借用会同步归还；若底层归还自身失败则按 host bridge 的错误回调合同处理。

此接口支持 SYSTEM 和 ARENA 域，只接受已有 Compute lease 和同一 Compute domain 的 session；不会修改 capability gate。CPU reference 只用于验证传输合同，不代表 GPU 驱动或性能已验证。
