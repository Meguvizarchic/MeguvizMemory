# Memory 与 Compute 缓冲传输

`<mgmem/compute_transfer.h>` 提供 C 接口 `mgmem_upload(&transfer)` 与 `mgmem_download(&transfer)`，C++ 可使用 `mgmem::compute::upload(transfer)` 与 `mgmem::compute::download(transfer)`，链接目标为 `MeguvizMemoryComputeHostBridge::host_bridge`。

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

上传需要目标为 `WRITE` lease；下载需要来源为 `READ` lease。传输复用既有 Compute `COPY` 通道：在 Vulkan 路径下会先进行中转分配和 `memcpy`，再执行上传或下载，因此**不是 GPU 内存零拷贝**，也不承诺性能提升。Vulkan 路径要求长度与两侧 lease 偏移量为 4 字节整数倍，GPU 侧偏移量还需满足设备对齐要求。`AHardwareBuffer` 不支持该 `COPY` 路径；异步任务即使已被接收，仍可能因对齐、设备丢失或不支持的资源类型而失败，须检查 `MgcCompletion` 的终态与状态码。

无论提交成功还是失败，传入的 Compute `lease` 始终由调用方持有，桥只为本次提交保留额外引用。Memory 缓冲的读/写借用会一直保活到提交引用归还——即 GPU 工作已结束、主机后处理和完成回调均返回之后。完成回调可能在函数返回前就被触发，因此其上下文、Memory 域以及错误回调上下文都必须保持存活，且回调不得抛异常。在任务执行结束前禁止访问提交出去的 `lease`；`mgc_ticket_query` 显示完成并不代表回调已退出，必须调用 `mgc_session_drain` 确认提交引用完全归还（严禁在该 `session` 的完成回调内调用 `drain`，否则 Compute 会返回 `BUSY`）。若提交同步失败且未返回 `ticket`，Memory 借用会立即同步归还；若归还自身失败，则按 host bridge 的错误回调合同处理。

## 适用范围与验证说明

本接口支持 System 和 Arena 域，仅接收已有的 Compute `lease` 与同一 Compute 域的 `session`，不会修改原有 capability gate。CPU reference 实现仅用于验证传输合同，不代表真实 GPU 驱动或性能已获验证。
