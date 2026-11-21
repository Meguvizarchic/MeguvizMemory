# Memory → Compute CPU 资源桥

短名入口和资源释放示例见 [创建 Compute 主机资源](SHORT_NAMES.md)。

本可选桥以 Memory 的读借用或写借用创建 Compute host 资源。借用会持续保持到 Compute 资源、租约与已提交任务的最后一份引用释放完毕，导入过程不复制缓冲区。调用方必须保证 Memory 域与错误回调上下文在该生命周期内保持存活，并严格遵守 `READ`/`WRITE` 读写互斥合同。

本桥依赖支持 `mgc_resource_import_host_v1` 的 Compute SDK 与 Memory SDK，不会改变普通 Compute 资源的原有创建路径。GPU 执行阶段可沿 Compute 既有的上传和下载通道使用该 CPU backing，设备同步与 GPU 资源释放仍由 Compute 负责管理。

异步数据传输接口与所有权说明见 [上传与下载](TRANSFER.md)；需要复用传输准备阶段时见 [复用传输准备](REUSE.md)。

当 Memory 最后一个借用归还失败时，错误回调会在释放线程触发，桥会保留不确定状态的上下文。此时调用方应停止使用受影响的域和资源，并保留其生命周期以便处理失败状态。
