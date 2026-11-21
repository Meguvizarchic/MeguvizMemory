# Memory 与 GPU-only AHardwareBuffer 写入

`<mgmem/gpu_ahb.h>` 提供受控的跨库写入协调：从 Memory 取得 READ 借用，向 Compute 预约完整 AHB WRITE lease，并将 Memory 指针、AHB 与前序 `ready_fd` 交给调用方自有的 GPU 执行器。支持不带 CPU 读写用途的 AHB，内部从不对 AHB 执行 CPU lock，也不会将 GPU 内存伪装成 CPU 地址。

```cpp
auto r = mgmem::gpu_ahb::begin(memory, allocation, offset, length, write_lease, on_error, error_context);
if (r.status == MGMEM_OK) {
    // 执行器读取 r.view.data，将内容上传或渲染到 r.view.buffer。
    // 它必须等待或导入 r.view.ready_fd，并完成所需的 GPU 内存同步。
    // 此处为伪代码：执行器必须分别报告是否提交、是否物理完成以及 fence。
    auto submitted = submit_gpu_write(r.view);
    int32_t compute_status = 0;
    if (submitted.accepted && (submitted.fence_fd >= 0 || submitted.physically_complete)) {
        const int producer_fd = submitted.physically_complete ? -1 : submitted.fence_fd;
        auto status = mgmem::gpu_ahb::publish(r.pending, producer_fd, &compute_status);
        // publish 会复制借用的 producer_fd；成功后调用方可关闭自身 fd。
        // 若失败则保留 fd 与 token，并以同一完成 fence 重试。
        if (status == MGMEM_OK) {
            // 超时或 fence 出错后保留 token，在合适线程重试。
            status = mgmem::gpu_ahb::retire(&r.pending, 100);
        }
    } else if (submitted.definitely_not_submitted) {
        // 仅当确认没有任何 GPU 工作提交时才可取消。
        mgmem::gpu_ahb::cancel(&r.pending, &compute_status);
    } else {
        // 提交与完成状态不明：隔离 r.pending，保留关联资源。
    }
}
// 无论返回何种状态，非空 r.pending 均归调用方持有直到 retire 成功或确认隔离；
// 执行器提交状态不明时严禁调用 cancel。
```

上例的 `submit_gpu_write` 为调用方执行器伪代码；本适配器本身不实现 GPU 上传或渲染逻辑。执行器必须遵守 Compute 的 `GENERAL` 布局与 `FOREIGN` 队列所有权合同：等待 `ready_fd`、完成图像同步并交回 `GENERAL` / `FOREIGN`，同时导出覆盖全部工作的有效 sync-file fence（`ready_fd` 借自 token，消费 fd 的 Vulkan/EGL 导入须先 `dup`）。同一 token 的调用必须串行。`publish(-1)` 断言所有 GPU 工作已物理完成，执行器错误或缺少 fence 均不可当作完成证明。

`publish` 向 Compute 注册生产者 fence，同时保留独立 fd 供 `retire` 确认 GPU 已停止读取 Memory。**Compute 接收 fence 不代表 Memory 缓冲区可以复用**，只有 `retire` 成功才释放 Memory 借用。若 `retire` 超时或 fence 报错，会保留 token 供重试；若最终 Memory 释放失败，则触发错误回调并隔离 token。调用方须保证 Memory 域、错误回调及其上下文存活至 `retire` 结束，并在执行器不再使用 `view` 后才销毁资源，同时禁止其他 AHB 别名并发访问。

底层 `publish` 允许在生产者 fence 尚未 signal 时提交给 Compute；调用方必须确保下游消费方能够正确处理 fence 错误，或先核验 fence 成功后再发布。`retire` 在 `poll` 就绪后会校验 sync-file 状态，仅当状态明确成功时才释放 Memory 借用；若仍处于 active、失败或系统不支持状态查询，则保留 token 及关联资源（内置 GLES 上传采用先验证生产者成功再发布的路径）。

仅在明确**没有提交任何 GPU 工作**时才可调用 `cancel`，它把原 `ready_fd` 归还 Compute（失败时保留 token 重试）。若 Compute 外部预约返回硬错误，可能已有所有权转移在进行，适配器会返回一个处于隔离态的 token 并继续持有双方引用，该类 token 不能经由本 API 安全回收；`BUSY` 或背压拒绝不会生成 token，调用方可稍后重试。传入的 Memory `owner` 与 Compute `lease` 须在 `begin` 返回前保持存活。

## 适用范围与验证说明

本模块支持 Android API `26+`，链接目标为 `MeguvizMemoryGpuAhbBridge::gpu_ahb`；目前仅支持 Compute 能力门允许的完整单层 RGBA8 WRITE lease。宿主测试使用模拟 Compute 与模拟 fence，只用于验证协议状态机，不代表真实 Android GPU 驱动或零拷贝性能已获验证。
