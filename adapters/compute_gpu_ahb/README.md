# Memory 与 GPU-only AHardwareBuffer 写入

`<mgmem/gpu_ahb.h>` 提供受控的跨库写入协调：从 Memory 取得 READ 借用，向 Compute 预约完整 AHB WRITE lease，把 Memory 数据、AHB 和前序 ready fence 一起交给调用方已有的 GPU 执行器。支持不带 CPU 读写用途的 AHB；不会对 AHB 调用 CPU lock，也不会把 GPU 内存伪装为 CPU 地址。

```cpp
auto r = mgmem::gpu_ahb::begin(memory, allocation, offset, length, write_lease, on_error, error_context);
if (r.status == MGMEM_OK) {
    // 执行器读取 r.view.data，将内容上传或渲染到 r.view.buffer。
    // 它必须等待/导入 r.view.ready_fd，并执行所需的 GPU 内存同步。
    // 这是伪代码：执行器必须分别报告是否提交、是否物理完成和 fence。
    auto submitted = submit_gpu_write(r.view);
    int32_t compute_status = 0;
    if (submitted.accepted && (submitted.fence_fd >= 0 || submitted.physically_complete)) {
        const int producer_fd = submitted.physically_complete ? -1 : submitted.fence_fd;
        auto status = mgmem::gpu_ahb::publish(r.pending, producer_fd, &compute_status);
        // publish 借用 producer_fd；成功后调用方可关闭自己的 fd。
        // 若失败则保留 fd 和 token，并以同一完成 fence 重试。
        if (status == MGMEM_OK) {
            // 超时或 fence 错误后保留 token，在合适线程重试。
            status = mgmem::gpu_ahb::retire(&r.pending, 100);
        }
    } else if (submitted.definitely_not_submitted) {
        // 仅当确认没有任何 GPU 工作提交时才可取消。
        mgmem::gpu_ahb::cancel(&r.pending, &compute_status);
    } else {
        // 提交与物理退休状态不明：隔离 r.pending，保留其关联的资源。
    }
}
// 无论返回状态如何，非空 r.pending 都由调用方保存，直到成功 retire
// 或被明确隔离；执行器提交状态不明时绝不能调用 cancel。
```

上例中的 `submit_gpu_write` 及其结果字段是调用方执行器的伪代码，本适配器不实现 GPU 代码，也不承诺零拷贝。执行器必须遵守 Compute 的 GENERAL 布局 / FOREIGN 队列所有权合同：消费 ready fence，完成自己的导入、上传或着色和必要的 GPU 同步，再把图像交回 GENERAL / FOREIGN，返回覆盖全部这些工作的有效 sync-file fence。`ready_fd` 借用自 token；消耗 fd 的 Vulkan/EGL 导入必须先 `dup`。同一 token 的操作须串行。`publish(-1)` 表示调用方确认所有工作已经物理完成，执行器错误或无 fence 均不能代替该证明。

`publish` 向 Compute 注册 producer fence，同时保留独立 fd 供 `retire` 证明 GPU 不再读取 Memory。不能以 Compute 已接收 fence 作为 Memory 已可复用的证据。只有成功 `retire` 才归还 Memory 借用。`retire` 超时或 fence 错误保留 token，可重试；确认的最终 Memory 释放失败会调用错误回调并隔离 token。调用者保证 Memory domain、错误回调及上下文存活至退役，且执行器不再使用 `view` 后才结束其生命周期。禁止其他 AHB 别名并发访问。

低层 `publish` 保留原有异步合同：它可以在生产者 fence 尚未 signal 时交给 Compute。该入口不会保证下游消费方识别 fence 携带的错误；调用方必须自行确认下游能够处理错误，或先验证 fence 成功后再发布。`retire` 在 poll 就绪后还检查 sync-file 状态，只有状态明确成功才归还 Memory 借用；状态为 active、失败或系统不支持查询时继续保留 token 和关联资源。内置 GLES 上传使用确认生产者成功后再发布的路径。

只有明确**没有提交任何 GPU 工作**时才能 `cancel`，它把原 ready fence 还给 Compute，失败时保留 token。Compute 的外部预约若返回硬错误，可能已有所有权转换在退休中，适配器会返回隔离 token 并继续持有 Memory 借用和 Compute lease；此类 token 不可由本 API 安全回收。`BUSY` / `BACKPRESSURE` 预约失败不产生 token，调用者可以重试。调用方持有传入的 Memory owner 和 Compute lease 至 `begin` 返回。

Android API 26 起可构建，链接 `MeguvizMemoryGpuAhbBridge::gpu_ahb`。宿主测试采用 fake Compute 和 fence，只验证协议，不验证 Android GPU 驱动。AHB 格式受现有 Compute AHB 能力门限制，目前仅完整单层 RGBA8 WRITE lease。
