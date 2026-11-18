# 复用传输准备

对不变的上传数据或多次下载，`<mgmem/compute_stage.h>` 可一次准备并重复提交，省去每次重新创建 Memory 借用、Compute host resource 和 host lease。链接目标仍是 `MeguvizMemoryComputeHostBridge::host_bridge`。一次性调用继续使用 `mgmem_upload` / `mgmem_download`。

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

准备使用 `transfer` 的 `memory`、`allocation`、`offset`、`bytes`、`compute`、`on_error` 和 `error_context`。每次 send 使用 `session`、`device`、`request_id` 及完成回调字段；设备 lease 和返回的 ticket 仍由调用方持有。Compute 原有能力门、COPY 的对齐和 AHardwareBuffer 限制均不变。

阶段持续占有 Memory 借用。上传阶段持有 READ，内容必须在整个阶段内不变；需要逐帧更改上传内容时，请使用一次性接口或先关闭并重新准备。下载阶段持有 WRITE，完成后通过 `mgmem_stage_map` / `mgmem_stage_unmap` 安全访问数据；映射期间不允许提交或关闭。Memory 其他借用在阶段存活期间遵循普通的读写互斥。

单个阶段最多一个提交未完成；执行、取消、完成回调和上下文释放回调结束前，新的 send、map 或 close 会返回 BUSY。调用方必须串行化所有阶段 API 调用，并确保阶段至少存活到 send 返回；回调可在 send 返回前发生，不可假设回调延迟执行。关闭成功消费阶段句柄，但 Compute 可能仍在归还最后的提交引用；销毁 Memory domain 或错误回调上下文前，仍需在 session 回调外 drain。调用方原有的 device lease 始终属于调用方。

此接口适合多次传输均摊准备及最终关闭成本。它不会将 CPU 内存变成 GPU 映射，不承诺零拷贝或设备侧性能提升。
