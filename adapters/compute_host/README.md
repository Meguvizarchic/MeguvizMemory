# Memory → Compute CPU 资源桥

短名入口和资源释放示例见 [创建 Compute 主机资源](SHORT_NAMES.md)。

此可选桥以 Memory 的读或写借用创建 Compute host 资源。借用保持到 Compute 资源、租约与提交任务的最后引用释放，导入不复制缓冲。调用方必须保持 Memory domain 和错误回调上下文存活，并遵循 READ/WRITE 排他合同。

桥需要同时支持 `mgc_resource_import_host_v1` 的 Compute SDK 与 Memory SDK。普通 Compute 资源创建行为保持原有路径。GPU 执行可沿 Compute 的原有上传和下载路径使用此 CPU backing，设备同步和 GPU 资源归还仍由 Compute 管理。

异步数据传输的短名和所有权说明见 [上传与下载](TRANSFER.md)。

Memory 最后借用归还失败时，错误回调在释放线程调用，桥保留不确定上下文。调用方应停止使用受影响域和资源，并保留其生命周期以处理失败。

不变上传或重复下载可使用 [复用传输准备](REUSE.md)。

