# Compute CPU READ → Memory 桥（可选）

短名入口与完整资源释放示例见 [只读导入调用](SHORT_NAMES.md)。

该独立适配器只接收完整、已就绪（ready）的 Compute CPU READ lease，将实际映射作为 Memory System 域的一个只读外部 backing。适配器不提供 GPU/AHB 映射、部分范围导入、Memory → Compute allocator 注入或数据复制，也不改变 Memory core/AAR 的默认依赖；使用者必须自行提供兼容的 Compute SDK 与 Memory SDK。

本桥依赖 Memory 的 `mgmem_import_external_checked_v1` 符号与 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE` 能力。两者属于可选 ABI `1.1` 扩展，**并非所有 ABI `1.1` 或早期 Memory 构建都包含它们**。本桥作为静态库，在链接缺少该符号的 Memory SDK 时必须失败，也不支持在运行时替换为不含该符号的共享库；使用者必须部署同一份具备该能力的 Memory 运行库，不能仅依据版本号判断支持。

单独配置、构建与安装：

```sh
cmake -S adapters/compute -B build/compute-bridge \
  -DCMAKE_PREFIX_PATH="<Memory 安装前缀>;<Compute 安装前缀>" \
  -DCMAKE_INSTALL_PREFIX="<桥安装前缀>"
cmake --build build/compute-bridge --config Release
cmake --install build/compute-bridge --config Release
```

消费时同时传入上述三个安装前缀，通过 `find_package(MeguvizMemoryComputeReadBridge CONFIG REQUIRED)`（或显式指定 `COMPONENTS read_bridge`）查找并链接 `MeguvizMemoryComputeReadBridge::read_bridge`。工具链及 MSVC CRT 须与独立安装的 Compute SDK 一致。

若需把完整 Compute READ backing 导入 Memory Arena 域，改用 `find_package(MeguvizMemoryComputeReadBridge CONFIG REQUIRED COMPONENTS arena_read_bridge)`，链接独立的 `MeguvizMemoryComputeReadBridge::arena_read_bridge`，并调用 `mgmem_compute_import_arena_read_v1`。该目标沿用相同的映射保活与 checked release 合同，导入字节计入 Arena 域预算但不参与 Arena 子分配。它要求同一 Memory 运行库同时导出 `mgmem_import_external_arena_checked_v1` 并通告 `MGMEM_CAP_ARENA_EXTERNAL_IMPORT` 与 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE`；构建时会探测 SDK 声明与符号，缺失时不生成该目标（也可通过 `-DMGMEM_COMPUTE_BRIDGE_BUILD_ARENA=OFF` 显式关闭）。原有 System 桥保持独立目标与依赖，无需链接 Arena 扩展。

## 身份、权限和关闭

调用方必须为每份 backing 分配在整个 Memory 运行库范围内稳定、非零且不冲突的 `external_id`（跨所有其他适配器和普通外部导入），不要简单截断或哈希 Compute 的 identity tuple。同一份物理 backing 只允许由一个 Memory 运行库管理，不同虚拟地址映射到同一物理页无法由注册表自动识别。

调用方须持有来源 lease 直至导入调用返回；本桥会在 `map` 前独立 retain 一份 Compute lease，且 `map` 自身也会按 Compute 合同持有引用。只有确认 `unmap` 成功时才归还桥的独立引用，失败时则保留该引用以免 origin 被提前析构。外部 C ABI 函数和回调须严格遵守不抛异常约定。禁止其他代码解开桥的映射、通过别名修改只读数据，或提前卸载任一运行库与回调代码。Memory 的 `owner` 释放或 `begin_close` 不会撤销仍存活的 Memory lease，直到最后一个借用释放才会解开 Compute 映射；原资源、content 与 Compute 回收的其他约束仍由来源方负责。

必须提供线程安全、不抛异常且不重入同一 Memory 域的 `on_unmap_error` 回调，并保持其上下文存活至全部 Memory 引用结束。如果已被 Memory 接收的映射在最终 `unmap` 时失败，checked release 会使域进入隔离态并保留登记记录与计费，不作重试也不宣称归零；调用方应立即停止使用该 backing 并排查 Compute 侧故障。

checked release 存在保守误判边界：如果来源方在释放回调返回前已回收地址，而其他合法 provider 在该时间窗口内复用了同一地址，由于 Memory 尚未解除旧登记，会将其当作地址冲突而拒绝或隔离。目前没有与外部 allocator 的两阶段释放握手，不能把这种现象认定为来源方必定违约。

如果 Memory 在 `retain` 之前拒绝导入，适配器会自动回滚映射。若回滚阶段的 `unmap` 也失败，接口返回 `MGMEM_COMPUTE_ROLLBACK_QUARANTINED` 并通知回调（同时保留映射上下文），此时 Memory 从未接收该映射且**不保留核心记录**，调用方须停止使用该 backing 并跨运行库协调后续访问。遵守独占映射合同时正常路径下 `unmap` 不应失败，上述保护不能替代违约代码的安全性保证。

## 适用范围与验证说明

本桥功能检查独立于设备、性能与产品资格；仅编译通过或宿主测试通过，不代表家族产品、GPU、Android 设备或零拷贝性能资格。
