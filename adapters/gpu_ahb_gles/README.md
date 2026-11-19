# GPU-only AHardwareBuffer 内置上传

`<mgmem/ahb_upload.h>` 提供 `mgmem::ahb::upload(desc)`，将 Memory 的完整 RGBA8 画面写入 Compute 的 GPU-only AHB WRITE lease。实现等待并核验 Compute ready fence，然后将 AHB 创建为 EGLImage、绑定到新的 GLES 2D 纹理，以 `glTexSubImage2D` 覆盖整个图像；提交后创建并导出 Android native fence。AHB 从不进行 CPU lock。输入的 `source_stride` 是 Memory 源行距，单位字节，必须能被 4 整除；与 AHB 自身行距无关。源第 0 行对应 GL 纹理的第 0 行，不进行翻转或颜色转换。新版 Compute 的可选受保护提前排队方式见 [guarded 交接](GUARDED.md)。

```cpp
mgmem::ahb::upload_desc desc{};
desc.memory = memory;
desc.allocation = allocation;
desc.offset = offset;
desc.length = bytes;
desc.source_stride = source_stride;
desc.width = width;
desc.height = height;
desc.write_lease = write_lease;
desc.ready_timeout_ms = 100;
desc.on_error = on_error;
desc.error_context = error_context;
auto result = mgmem::ahb::upload(desc);
// 检查 result.status 和 result.phase，并持有非空 result.pending。
if (result.status == MGMEM_OK) {
    auto status = mgmem::ahb::wait(&result.pending, 100);
    // 超时或 BUSY 时保留 result.pending 并稍后重试；
    // fence 错误或无法核验时按 phase 保留并隔离，不得当作成功。
}
```

准备阶段同步等待 ready fence，超时不会在后台继续上传。成功提交后 GPU 异步执行，token 处于 PRODUCING 且继续独占 Compute 外部预约；调用方显式 `wait` 才检查生产者 fence 的成功状态，在同一线程的原 EGL 上下文中销毁纹理、EGLImage 和 EGLSync，再将已完成结果交给 Compute，最后归还 Memory 借用及 Compute lease。清理未确认时不会向 Compute 发布。此设计不会将等待移交后台，但下游 Compute 无法在上传完成前提前排队。不得把仅有 fence signal 当作成功：错误状态或无法查询状态时隔离 token，不发布目标内容。不要在任务未退役时销毁 Memory domain、错误回调上下文或 EGL 上下文；同一 token 的所有 API 调用必须由调用者串行化。

非空 `pending` 始终属于调用方，可用 `mgmem_ahb_upload_phase` 查询后续状态。`MGMEM_UPLOAD_CANCEL` 表示没有提交图像操作，可调用 `mgmem_ahb_upload_cancel` 重试归还；`MGMEM_UPLOAD_PRODUCING` 等待 GPU 完成并核验状态；`MGMEM_UPLOAD_PUBLISH` 表示生产者已验证成功但 Compute 登记失败，可调用 `mgmem_ahb_upload_retry` 或再次 `wait`；`MGMEM_UPLOAD_RETIRE` 允许调用 `mgmem_ahb_upload_wait`。`MGMEM_UPLOAD_QUARANTINED` 表示目标、GPU 退休或最终归还状态未知，必须保留 token 和所有关联对象，不能以 `-1` 充当完成证明。

需要当前线程的 GLES3 EGL 上下文，以及 `EGL_KHR_image_base`、`EGL_ANDROID_get_native_client_buffer`、`EGL_ANDROID_image_native_buffer`、`EGL_KHR_fence_sync`、`EGL_ANDROID_native_fence_sync` 和 `GL_OES_EGL_image`。缺少扩展时返回不支持；实际设备也可能拒绝修改 EGLImage 纹理，不做隐式降级。目标必须是现有 Compute 能力门允许的完整单层 RGBA8 WRITE lease。上传只在当前活动纹理单元绑定临时纹理，并恢复原纹理绑定。当上下文的 unpack PBO 或像素解包参数与源行距不匹配时，临时调整这些设置，结束后恢复。进入调用前应清空 GL/EGL 错误队列。OOM、context lost、状态恢复失败及无法确认的提交均进入隔离状态。

ready 和生产者 fence 通过 poll 后分别查询 sync-file 状态；现代 UAPI 不支持时仅在 ENOTTY 下回退 Android 旧版 sync UAPI，二者均无法验证或报告错误时保持隔离。若 status ioctl 被信号反复中断直至超时，生产者阶段保留 token 并允许重试；ready 阶段因无法确认状态而隔离，不将其退还给可能只检查 signal 的 Compute。该模块是可选 Android 静态适配器，链接 `MeguvizMemoryAhbGlesUpload::upload`。它有明确的输入 fence 同步等待、驱动上传与 GPU 退休成本；没有设备性能或驱动兼容性保证。调用方必须保证 AHB 没有其他并发别名访问，并遵守 Compute 的外部所有权合同。Memory、Compute 与适配器的代码映像须保持加载直至最后 token 退役或隔离状态被安全处理。
