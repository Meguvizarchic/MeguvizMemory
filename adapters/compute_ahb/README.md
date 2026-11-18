# Compute AHB → Memory CPU 映射

短名入口和恢复句柄处理示例见 [AHardwareBuffer 映射](SHORT_NAMES.md)。

连续映射时，可选的 [复用映射槽](REUSE.md) 复用桥状态对象；每次仍单独预约、同步和解锁 AHB。

独立适配器接收 Compute 的完整 AHardwareBuffer READ lease。它先向 Compute 预约外部访问，等待返回的生产者 fence，再锁定 AHB 的 CPU 只读映射。最后一个 Memory 借用归还时，同步完成 AHB 解锁，再结束 Compute 预约。

`mgmem_ahb_write` 接收完整的 Compute WRITE lease，采用同样的 fence、预约及同步解锁流程，以 Memory 可读写的外部 backing 导入。Memory 所有 owner 和借用必须归还后才能发布或再次使用 Compute lease；写入未初始化的数据时，调用方负责初始化需要的区域。由于 Memory WRITE owner 允许 READ 借用，AHB 必须同时声明 CPU READ 和 CPU WRITE 用途，仅支持写用途的 buffer 会被拒绝。

支持带 CPU READ 用途的单层 RGBA8 AHB。Memory 导入的容量是 `row_stride × height` 的 CPU 可访问范围，包含行填充，不能作为 GPU 实际分配大小。返回的 `compute_bytes` 是 Compute 的原始容量，两者各自记账时需避免加总为物理占用。其他格式、受保护内容及不符合范围合同的 lease 返回不支持。

`timeout_ms` 控制成功取得外部预约后返回的 ready fence 的等待时间，`0` 为非阻塞，`-1` 表示等待直到完成。Compute lease 初始 acquire 依赖尚未就绪时，预约本身可能直接返回 BUSY，不受此超时控制；调用方可先使用 `mgc_android_wait_ready` 或在 BUSY 后重试。该调用和最后一次归还可能同步等待，不适合对阻塞敏感的线程。

入口接受调用方分配的稳定 `external_id`。同一物理 backing 的跨 runtime 和异址别名仍需调用方协调；保留原 Compute 内容 lease 直至导入返回，避免其他外部访问与本次映射重叠。关联运行库和错误回调须保持有效直至最终归还。

在 Memory 接纳前，若等待超时、锁定或预算失败，适配器归还预约；若预约归还失败，返回 `recovery` 句柄，可稍后调用 `mgmem_ahb_recover` 重试。若 fence 明确出错或无法检查其状态，或解锁状态未知，适配器保留预约、句柄与来源引用并隔离，不能重试归还。Memory 已接纳后发生归还失败，checked release 保留不确定状态并通知回调。

该适配器依赖 Android API 26、Compute 的 Android AHB 接口以及支持 checked external release 的 Memory runtime。构建提供两个 SDK 前缀：

```sh
cmake -S adapters/compute_ahb -B build/ahb-bridge -DCMAKE_PREFIX_PATH="<Memory>;<Compute>"
cmake --build build/ahb-bridge --config Release
```

安装后使用 `find_package(MeguvizMemoryAhbReadBridge CONFIG REQUIRED)` 和 `MeguvizMemoryAhbReadBridge::read_bridge`。宿主 fake 测试只验证调用与错误路径，Android 设备、驱动与性能需另行验证。
