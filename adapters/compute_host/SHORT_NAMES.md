# 创建 Compute 主机资源

包含 `<mgmem/compute_host.h>`，C++ 可用 `mgmem::compute::host` 创建资源。成功后调用方持有返回的 Compute 资源，失败时 `resource` 为空，`origin_status` 保留同步 Compute 状态。

```cpp
#include <mgmem/compute_host.h>

mgmem_status create_and_release(mgmem_domain *memory, mgmem_handle_v1 allocation,
                                MgcDomain *compute_domain, const MgcResourceDesc *desc,
                                mgmem_compute_host_error_fn on_error, void *context) {
    auto result = mgmem::compute::host(memory, allocation, 0, compute_domain, desc, MGC_ACCESS_READ, on_error, context);
    if (result.status != MGMEM_OK) return result.status;
    mgc_resource_release(result.resource);
    return MGMEM_OK;
}
```

C 调用使用 `mgmem_compute_host`。调用方须保持 Memory domain 和错误回调上下文有效，直到 Compute 资源及其租约和任务的最后引用归还。发生异步归还失败时，回调会报告该失败。
