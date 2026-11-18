# AHardwareBuffer CPU 映射

包含 `<mgmem/ahb_map.h>` 可用 `mgmem::ahb::read` 一次取得状态、Memory owner、布局以及可能需要保留的恢复句柄。入口同步等待预约返回的 ready fence；Compute lease 的初始 acquire 依赖尚未就绪时，预约可能直接返回 BUSY。超时时间及所有权合同与 `mgmem_ahb_read` 相同。

```cpp
#include <mgmem/ahb_map.h>

// 对于需要恢复的句柄，由调用方保留到恢复或隔离处理完成。
void preserve_recovery(mgmem_ahb_recovery *, int32_t stage, int32_t status);

mgmem_status import_and_release(mgmem_domain *memory, MgcLease *lease, uint64_t external_id,
                                mgmem_ahb_error_fn on_error, void *context) {
    auto result = mgmem::ahb::read(memory, lease, external_id, 100, on_error, context);
    if (result.status != MGMEM_OK) {
        if (result.recovery) preserve_recovery(result.recovery, result.error_stage, result.error_status);
        return result.status;
    }
    return mgmem_release_v1(memory, result.owner);
}
```

需要读取数据时，在释放 owner 前申请 Memory READ 借用，并在读取完成后归还。恢复句柄可按原合同调用 `mgmem_ahb_recover`，解锁状态未知时由调用方保持隔离。回调及其上下文须保持有效，直到 owner 和所有借用结束。C 入口为 `mgmem_ahb_map`。

写入使用 `mgmem::ahb::write(memory, lease, external_id, 100, on_error, context)`；C 入口为 `mgmem_ahb_map_write`。它返回同一种结果结构，错误与恢复处理相同。Compute lease 必须是 WRITE，AHB 必须同时具有 CPU READ 和 CPU WRITE 用途。取得 Memory WRITE 借用并完成写入后，先归还所有借用与 owner，再发布或使用 Compute lease。最后一次归还可能同步等待 AHB 解锁；该接口不初始化缓冲内容。
