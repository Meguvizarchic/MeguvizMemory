# Compute 与 Media 只读导入

包含 `<mgmem/compute_read.h>` 可使用短名称。调用仍要求完整、已就绪的 CPU READ 租约、全 runtime 唯一的 `external_id`，以及有效的错误回调和上下文。返回的 `owner` 由调用方释放；`origin_status` 保留同步 Compute 错误。错误回调的上下文须在最后一个 Memory 借用结束前保持有效。

```cpp
#include <mgmem/compute_read.h>

// on_error 和 context 由调用方提供，external_id 由调用方协调。
mgmem_status check_and_release(mgmem_domain *arena, MgcLease *lease,
                               uint64_t external_id,
                               mgmem_compute_unmap_error_v1 on_error, void *context) {
    auto result = mgmem::compute::read_arena(arena, lease, external_id, on_error, context);
    if (result.status != MGMEM_OK) return result.status;
    return mgmem_release_v1(arena, result.owner);
}
```

需要读取内容时，在释放 owner 前使用 `mgmem_borrow_acquire_v1` 取得 READ 借用，并在读取完成后调用 `mgmem_borrow_release_v1`。异步工作应持有自己的借用直至完成。

System 域使用 `mgmem::compute::read`。C 调用分别使用 `mgmem_compute_read` 和 `mgmem_compute_read_arena`，同样返回 `mgmem_read_result`。Arena 入口需链接 `MeguvizMemoryComputeReadBridge::arena_read_bridge`，System 入口链接 `MeguvizMemoryComputeReadBridge::read_bridge`。

Media 帧可包含 `<mgmem/media_read.h>`，使用 `mgmem::media::read`、`mgmem::media::read_arena`，或 C 的 `mgmem_media_read`、`mgmem_media_read_arena`。调用方在函数返回前持有帧引用，结果 owner 和错误处理遵循上述相同流程。Media 入口需要相应的 Media 桥目标。
