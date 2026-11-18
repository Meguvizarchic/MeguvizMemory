# 可选的受保护异步交接

仅在明确使用支持 guarded producer fence 的新版 Compute SDK 时，将适配器配置为 `MGMEM_AHB_GLES_GUARDED=ON`，并链接 `MeguvizMemoryAhbGlesUpload::guarded`。原 `::upload` target 默认不依赖新的 Pack 符号，旧 SDK 和原先的同步确认路径保持可用。

```cpp
#include <mgmem/ahb_upload_guarded.h>

auto result = mgmem::ahb::upload(desc);
if (result.status == MGMEM_OK) {
    int32_t compute_status = 0;
    auto status = mgmem::ahb::queue(result.pending, compute_domain, &compute_status);
    if (status == MGMEM_OK) {
        // 可发布同一 WRITE lease 的 READ 引用，并向支持该 lease 的下游排队。
        // 下游真正访问前仍受 producer fence 和清理 gate 双重约束。
    }
    // 无论是否 queue，原 GL 线程都必须稍后 wait；超时保留 pending。
    // 确认成功后 wait 清理 GL/EGL，才为该 generation 打开 gate。
}
```

`queue` 核对 domain 与 lease 的 id/代数，并要求 capability 19 已通告、启用且 `probe_result` 为成功。domain、lease 和 guarded target 必须来自同一个已加载 Compute runtime。能力缺失或 domain 不匹配时不会注册 fence，调用方可继续使用原 `wait` 安全路径；不进行不安全的自动回退。

producer fence 明确报错或 GL/EGL 清理发生硬失败时，适配器向同一 guard generation 发送失败回执。Pack 负责让已接受任务失败并拒绝该内容；Memory 仍保留 token、lease 和 FD，直到可证明安全。失败回执遇 BUSY 时 token 进入 `MGMEM_UPLOAD_FAIL_NOTIFY`，调用 `mgmem_ahb_upload_retry` 仅重试通知，成功只代表通知已送达，token 随后仍处于隔离状态。状态无法核验不得伪报为已确认的 producer 错误，仍需保留隔离；调用方也应取消自己持有的下游任务。

该选项允许提前将任务交给 Pack 排队；不宣称 GPU 在 fence/清理完成之前执行。调用方仍须显式完成 wait、清理和 gate 确认；Pack 接受后可能在其工作线程等待依赖，完整延迟与资源占用需单独测量。producer 未验证前，源 Memory 借用、AHB、GL/EGL 对象和代码映像均须继续存活；所有 token 操作由调用方串行化。旧 SDK 不能链接该可选 target。
