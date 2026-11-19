# Compute CPU READ → Memory 桥（可选）

短名入口和完整资源释放示例见 [只读导入调用](SHORT_NAMES.md)。

该独立构建仅接收完整、ready 的 Compute CPU READ lease，将实际映射作为 Memory SYSTEM 域的一个只读外部 backing。这里没有 GPU/AHB 映射、部分范围导入、Memory → Compute allocator 注入或数据复制。它不改变 Memory core/AAR 的默认依赖；使用者必须自行提供真实且互相兼容的 Compute SDK 和 Memory SDK。

适配器要求 Memory 的 `mgmem_import_external_checked_v1` 符号及 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE` 能力。两者是可选 ABI 1.1 扩展，并非所有 1.2.1/ABI 1.1 Memory 构建都包含它们。桥为静态库，与缺少该符号的 Memory SDK 链接时必须失败；运行时换入不含符号的共享库也不支持。使用者须部署同一个、具有该能力的 Memory runtime，不能仅依据版本号判断支持。

单独配置、构建、安装：

```sh
cmake -S adapters/compute -B build/compute-bridge \
  -DCMAKE_PREFIX_PATH="<Memory安装前缀>;<Compute安装前缀>" \
  -DCMAKE_INSTALL_PREFIX="<桥安装前缀>"
cmake --build build/compute-bridge --config Release
cmake --install build/compute-bridge --config Release
```

消费时同时提供三个安装前缀，用 `find_package(MeguvizMemoryComputeReadBridge CONFIG REQUIRED)` 查找默认 System 桥，或在调用中显式指定 `COMPONENTS read_bridge`。链接目标为 `MeguvizMemoryComputeReadBridge::read_bridge`。工具链及 MSVC CRT 应与独立安装的 Compute SDK 匹配。

需要把完整 Compute READ backing 导入 Memory Arena 域时，使用 `find_package(MeguvizMemoryComputeReadBridge CONFIG REQUIRED COMPONENTS arena_read_bridge)`，链接独立的 `MeguvizMemoryComputeReadBridge::arena_read_bridge`，调用 `mgmem_compute_import_arena_read_v1`。它沿用相同映射保活与 checked release 合同，并计入该 Arena 域的预算；导入的 backing 不参与 Arena 子分配。该目标要求同一 Memory runtime 同时提供 `MGMEM_CAP_ARENA_EXTERNAL_IMPORT`、`MGMEM_CAP_CHECKED_EXTERNAL_RELEASE` 和 `mgmem_import_external_arena_checked_v1`。构建时检测所选 SDK 的声明和符号，缺失时不生成该目标，也可通过 `MGMEM_COMPUTE_BRIDGE_BUILD_ARENA=OFF` 显式关闭。原 SYSTEM 桥保留独立目标与原依赖，使用旧 runtime 的调用者无需链接 Arena 扩展。

## 身份、权限和关闭

调用者必须给每份 backing 分配全 Memory runtime 范围内稳定、非零且不冲突的 `external_id`，包括其他 adapter 和普通 external import；不要截断或简单哈希 Compute 的 identity tuple。调用者也负责保证同一物理 backing 只由一个 Memory runtime 管理；不同虚拟地址映射到同一物理页不能由 registry 自动识别。

调用者需持有来源 lease 直至导入返回；桥在 map 前另行 retain 一份 Compute lease，map 也会按 Compute 合同自持引用。只有确定 unmap 成功才归还桥的独立引用；unmap 失败时保留它，避免误操作提前消费 map 引用后 origin 被析构。外部 C ABI 函数和回调必须遵守 no-throw 合同，违约抛异常不是可保证恢复的路径。必须禁止其他代码解开桥的 mapping、通过别名修改只读内容、或提前卸载任一 runtime 与回调代码。Memory 的 owner 释放或 begin_close 不会使仍然存活的 Memory lease 失效；最后一次释放才会解开 Compute map。原资源、content 与 Compute retirement 的其他约束仍由来源负责。

必须提供线程安全、不得抛异常及不得重入同一 Memory domain 的 `on_unmap_error` 回调；其上下文要活到所有 Memory 引用结束。若一个已被 Memory 保留的映射在最终 unmap 时失败，checked release 使域 poison，保留其 claim 和预算，不重试并保留外部状态；成功归零不可声称。调用方也应立即停止使用该 backing，处理 Compute 侧故障。

checked release 也有保守误判边界：若 origin 在释放回调返回前已释放存储，其他合法 provider 在这一窗口复用相同地址时，Memory 尚持有旧 claim，可能将重叠拒绝或隔离。当前没有与外部 allocator 的两阶段释放握手；不能把这种结果称为 origin 一定违约。

若 Memory 在 retain 前拒绝导入，适配器自行回滚 mapping。若回滚的 unmap 也失败，函数返回独立的 `MGMEM_COMPUTE_ROLLBACK_QUARANTINED` 并通知回调，映射上下文保留。但此时 Memory 从未接纳该 mapping，**没有可保留的 core claim**；调用方须禁止该 backing 的后续操作并跨所有 runtime 协调，不能声称 adapter 防止了违约方重新导入。当前 Compute 实现中，在遵守独占 mapping 合同的正常路径，unmap 不应失败；上述处理不是对任意违约代码的安全保证。

功能检查与设备/性能/产品资格分开；仅构建本项目或通过 host 检查，不代表家族产品、GPU、Android 或零拷贝性能资格。
