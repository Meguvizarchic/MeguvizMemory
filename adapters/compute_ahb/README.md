# Compute AHB → Memory CPU 映射

短名入口和恢复句柄处理示例见 [AHardwareBuffer 映射](SHORT_NAMES.md)；需要连续多次映射时，可通过 [复用映射槽](REUSE.md) 复用桥状态对象（每次仍单独预约、同步和解锁 AHB）。

本独立适配器接收 Compute 的完整 `AHardwareBuffer` READ lease：先向 Compute 预约外部访问并等待返回的生产者 fence，再锁定 AHB 的 CPU 只读映射；当最后一个 Memory 借用归还时，先同步完成 AHB 解锁，再结束 Compute 预约。

`mgmem_ahb_write` 则接收完整的 Compute WRITE lease，沿用相同的 fence、预约及同步解锁流程，将其作为可读写的外部 backing 导入 Memory。在发布或再次使用该 Compute lease 前，必须先归还全部 Memory `owner` 和借用；写入未初始化数据时由调用方负责初始化相应区域。由于 Memory WRITE `owner` 允许派生 READ 借用，目标 AHB 必须同时具备 CPU READ 与 CPU WRITE 用途（仅声明写用途的 buffer 会被拒绝）。

当前仅支持带 CPU READ 用途的单层 RGBA8 AHB。Memory 导入记账的容量是 `row_stride × height` 的 CPU 可访问范围（含行填充，不可当作 GPU 实际分配尺寸）；结构体返回的 `compute_bytes` 为 Compute 原始容量，两者不可相加充当总物理占用。其余格式、受保护内容或不符合范围约定的 lease 均返回不支持。

`timeout_ms` 控制取得外部预约后等待 `ready` fence 的超时时间（`0` 为非阻塞，`-1` 为等待直至完成）。若 Compute lease 的初始 acquire 依赖尚未就绪，预约阶段可能直接返回 `BUSY`（不受 `timeout_ms` 控制），调用方需先调用 `mgc_android_wait_ready` 或在 `BUSY` 后重试。由于导入与最终释放均可能同步阻塞，不适合在对阻塞敏感的线程调用。

调用方须提供运行库全局唯一的 `external_id`，并协调同一物理 backing 的跨运行库别名；同时持有来源 Compute `lease` 直至导入返回，避免其他外部访问与当前映射重叠，并保持相关运行库与错误回调存活到最终归还。

在 Memory 接收之前，若因等待超时、锁定或预算不足失败，适配器会尝试归还预约（归还失败时输出 `recovery` 句柄，可调用 `mgmem_ahb_recover` 重试）。如果 fence 明确出错、无法查询其状态或解锁状态未知，适配器会保留预约、句柄与来源引用并进入隔离，禁止重试归还。Memory 接收后若在最终归还阶段出错，checked release 会保留不确定状态并触发错误回调。

本适配器依赖 Android API `26+`、Compute Android AHB 接口以及支持 checked external release 的 Memory 运行库。独立构建示例：

```sh
cmake -S adapters/compute_ahb -B build/ahb-bridge -DCMAKE_PREFIX_PATH="<Memory>;<Compute>"
cmake --build build/ahb-bridge --config Release
```

## 适用范围与验证说明

安装后通过 `find_package(MeguvizMemoryAhbReadBridge CONFIG REQUIRED)` 查找并链接 `MeguvizMemoryAhbReadBridge::read_bridge`。宿主 fake 测试仅覆盖调用与错误恢复路径，Android 真机、驱动兼容性与性能需另行验证。
