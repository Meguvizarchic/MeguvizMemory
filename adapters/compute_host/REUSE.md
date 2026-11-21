# 复用传输准备

针对上传内容不变或需多次下载的场景，`<mgmem/compute_stage.h>` 支持一次准备并重复提交，避免每次都重建 Memory 借用、Compute host resource 与 host lease，链接目标仍为 `MeguvizMemoryComputeHostBridge::host_bridge`。单次传输继续使用 `mgmem_upload` / `mgmem_download`。

```cpp
auto prepared = mgmem::compute::prepare_upload(transfer);
if (prepared.status == MGMEM_OK) {
    auto sent = mgmem::compute::send(prepared.stage, transfer);
    if (sent.status != MGMEM_OK) {
        mgmem_stage_close(&prepared.stage);
    } else if (mgc_session_drain(transfer.session, 5000000000ULL) == MGC_SUCCESS) {
        MgcCompletion completed{};
        completed.header = MGC_HEADER_INIT(MgcCompletion);
        auto query = mgc_ticket_query(sent.ticket, &completed);
        // 检查 query、completed.terminal 和 completed.status。
        mgc_ticket_release(sent.ticket);
        mgmem_stage_close(&prepared.stage);
    } else {
        // drain 未完成：保留 prepared.stage 和 sent.ticket，稍后确认并释放。
    }
}
```

准备阶段仅读取 `transfer` 中的 `memory`、`allocation`、`offset`、`bytes`、`compute`、`on_error` 和 `error_context`。每次 `send` 使用 `session`、`device`、`request_id` 及完成回调字段；设备 `lease` 与返回的 `ticket` 仍由调用方持有。Compute 原有的 capability gate、COPY 对齐以及 `AHardwareBuffer` 限制均保持不变。

`stage` 在整个存活期持续占用 Memory 借用：上传阶段持有 `READ` 借用，源内容必须保持不变（若需逐帧更新内容，请使用一次性接口或关闭后重新准备）；下载阶段持有 `WRITE` 借用，完成后可通过 `mgmem_stage_map` / `mgmem_stage_unmap` 安全读写，映射期间禁止调用 `send` 或 `close`。在 `stage` 存活期间，Memory 的其他借用仍遵守正常的读写互斥规则。

同一时间每个 `stage` 最多只允许一个未完成提交；在任务执行、取消、完成回调以及上下文释放回调彻底结束前，再次调用 `send`、`map` 或 `close` 会返回 `MGMEM_BUSY`。调用方必须串行化所有 `stage` 接口调用，并确保 `stage` 存活至 `send` 返回（注意完成回调可能在 `send` 返回前即已执行）。`mgmem_stage_close` 成功会消费句柄，但 Compute 侧可能仍在异步释放最后的提交引用，因此在销毁 Memory 域或错误回调上下文前，仍须在 `session` 回调外执行 `drain`。

## 适用范围与验证说明

本接口适合通过多次传输均摊准备与销毁开销；它不会将 CPU 内存变为 GPU 直接映射，也不承诺零拷贝或设备端性能提升。
