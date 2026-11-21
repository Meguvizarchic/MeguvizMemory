# GPU-only AHardwareBuffer 内置上传

`<mgmem/ahb_upload.h>` 提供 `mgmem::ahb::upload(desc)`，将 Memory 中的完整 RGBA8 图像写入 Compute 的 GPU-only AHB WRITE lease。实现会先等待并校验 Compute 的 `ready` fence，再把 AHB 创建为 `EGLImage` 并绑定到临时 GLES 2D 纹理，通过 `glTexSubImage2D` 覆盖整帧图像，提交后导出 Android native fence；全过程不对 AHB 调用 CPU lock。`source_stride` 为 Memory 源行距（单位字节，必须为 4 的整数倍，与目标 AHB 行距无关），源第 0 行对应 GL 纹理第 0 行，不执行翻转或色彩转换。若使用支持 guarded producer fence 的新版 Compute SDK，可参考 [guarded 交接](GUARDED.md)。

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
// 检查 result.status 与 result.phase，并持有非空 result.pending。
if (result.status == MGMEM_OK) {
    auto status = mgmem::ahb::wait(&result.pending, 100);
    // 超时或 BUSY 时保留 result.pending 并稍后重试；
    // fence 错误或无法核验时按 phase 保留并隔离，不得当作成功。
}
```

准备阶段同步等待 `ready` fence，超时不会把上传残留到后台。提交成功后 GPU 异步执行，token 进入 `MGMEM_UPLOAD_PRODUCING` 并继续独占 Compute 外部预约；调用方须在同一条原 EGL 线程上显式调用 `wait`，确认生产者 fence 成功状态、销毁纹理/`EGLImage`/`EGLSync`、向 Compute 发布已完成结果，最后再归还 Memory 借用与 Compute lease（清理未确认时不会向 Compute 发布）。不得仅凭 fence signal 判定成功：状态报错或无法查询时应隔离 token 而非发布内容。任务未结束前禁止销毁 Memory 域、错误回调上下文或 EGL 上下文，同一 token 的所有 API 调用须由调用方串行化。

调用方始终拥有非空的 `pending` 句柄，可通过 `mgmem_ahb_upload_phase` 查询后续阶段：`MGMEM_UPLOAD_CANCEL` 表示尚未提交图像操作，可调用 `mgmem_ahb_upload_cancel` 重试取消；`MGMEM_UPLOAD_PRODUCING` 等待 GPU 完成并核验状态；`MGMEM_UPLOAD_PUBLISH` 表示生产者已核验成功但 Compute 登记失败，可调用 `mgmem_ahb_upload_retry` 或再次 `wait`；`MGMEM_UPLOAD_RETIRE` 允许继续完成 `wait` 收尾；`MGMEM_UPLOAD_QUARANTINED` 表示目标、GPU 执行或最终释放状态未知，必须保留 token 与全部关联对象（不能用 `-1` 充当完成证明）。

运行要求当前线程已绑定 GLES3 EGL 上下文，并具备扩展 `EGL_KHR_image_base`、`EGL_ANDROID_get_native_client_buffer`、`EGL_ANDROID_image_native_buffer`、`EGL_KHR_fence_sync`、`EGL_ANDROID_native_fence_sync` 与 `GL_OES_EGL_image`；缺少扩展时返回不支持，部分设备驱动也可能拒绝修改 `EGLImage` 纹理（不做隐式降级）。实现仅临时修改当前活动纹理单元绑定及必要的像素解包参数并在结束时恢复；进入调用前应先清空 GL/EGL 错误队列。遇到 OOM、`context lost`、状态恢复失败或无法核验的提交均进入隔离态。

## 适用范围与验证说明

`ready` 与生产者 fence 在 `poll` 后分别校验 sync-file 状态（现代 UAPI 不支持时仅在 `ENOTTY` 下回退旧版 Android sync UAPI，均无法确认时保持隔离）。该适配器为可选 Android API `26+` 静态库（链接 `MeguvizMemoryAhbGlesUpload::upload`），存在明确的输入 fence 等待、驱动上传与 GPU 同步开销，不承诺设备性能或全驱动兼容性。Memory、Compute 与本适配器代码映像须保持加载直到最后一个 token 完成回收或隔离状态得到安全处理。
