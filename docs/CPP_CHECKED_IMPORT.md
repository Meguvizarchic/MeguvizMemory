# C++ 可检查外部导入

`mgmem::domain::import_external_checked` 是头文件形式的可选 C++17 包装，接收 `mgmem_external_buffer_checked_v1` 并返回现有的只可移动对象 `mgmem::owned_allocation`。它遵循完整外部区域导入、origin 保活与释放、单运行库路由、读写借用互斥以及软预算规则，不复制数据也不跨 ABI 暴露 C++ 容器。

该方法直接链接 `mgmem_import_external_checked_v1` 符号：消费方必须确保部署的同一份 Memory 运行库实际导出该符号并通告 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE`，**不能仅凭 ABI `1.1` 或版本号（如 `1.2.1` / `1.2.2`）假定支持**。若旧版运行库缺少该符号，加载时即会失败；需要运行时动态回退的场景，应先从目标运行库解析符号并检查 capability。

## 释放状态与隔离

- **正常消费**：显式调用 `owned_allocation::release()` 或 `borrow::release()` 返回 `MGMEM_OK` 时，包装对象被清空；对空包装重复调用 `release()` 幂等返回 `MGMEM_OK`。
- **释放失败进入隔离**：当 checked `owner` 或其派生的 `borrow` 在 `release()` 返回 `MGMEM_PROVIDER_ERROR` 时，C++ 层无法判断底层句柄是否已消费或外部释放是否发生，会立即清空该包装对象、**禁止重试该句柄**，并将共享 `domain` 标记为粘性 `quarantined`（隔离态）。其他错误码沿用原规则：包装对象保持有效，供调用方检查状态。
- **隔离后观测与限制**：`domain::observe()` 会顺序读取域快照、导入快照与粘性隔离标志（非原子快照，最后读取标志），在隔离后仍可安全调用且不改变状态；`domain::quarantined()` 用于查询这一粘性标志。进入隔离后，由 C++ 包装发起的新 `allocate`、`import`、`borrow`、`set_budget`、`reset`、`trim`、`collect` 和 `drain` 均会返回 `MGMEM_PROVIDER_ERROR`；`begin_close`、快照读取以及已有 `owner`/`lease` 的 `release()` 仍可调用，已授予的借用也不会被强制撤销。
- **并发窗口与检查时机**：隔离标志检查与进入 C 核心之间不是持锁事务；若其他线程在隔离标志置位前已通过检查，对应操作仍可能随后完成。需要严格截断时应由调用方在提交前协调，适配器不会跨外部回调持锁，也不会让 `begin_close` 阻塞等待用户回调。
- **导入自身失败的判定**：若 `import_external_checked` 返回 `MGMEM_PROVIDER_ERROR`，适配器会检查 `import_snapshot`；仅当确认既未 `poisoned` 且 `pending_import_bytes == 0` 时，按失败原子 `retain` 合同允许域继续使用；若快照读取失败、已 `poisoned` 或存在进行中的导入，则保守置为隔离态（并发场景下可能包含其他线程正在进行的导入）。
- **析构保留策略**：被隔离域的最后一个 C++ 包装销毁时，适配器会刻意保留底层 `domain`、账目和未确认记录，既不自动 `collect` / `destroy`，也不重试外部回调。由于析构函数与移动赋值无法返回错误状态，需要确切获知释放结果的调用方应在销毁前显式调用 `release()` 并保留 `domain` 包装以检查状态。一旦丢弃所有包装，该隔离域将无法通过本适配器找回；origin 上下文、回调代码及运行库在进程退出前必须保持加载有效。

未使用 checked 导入的普通 C++ 包装行为保持不变：其析构器要求底层清理必须成功，违约时触发 `std::terminate`；只有 checked 导入的 `MGMEM_PROVIDER_ERROR` 会转入粘性隔离，其他错误仍不可依赖析构忽略。本扩展不修改核心 C ABI 布局与旧接口语义。

## 适用范围与验证说明

`tests/cpp_checked/` 为独立宿主开发测试：使用真实核心覆盖正常释放、失败原子 `retain`、`owner` 与最后 `lease` 失败、析构、关闭竞争和并发包装操作，并使用 fake ABI 模拟返回 `MGMEM_PROVIDER_ERROR` 时句柄未消费、快照不可读及存在进行中导入等边界。真实失败用例会保留未确认资源并在独立进程退出，不代表无泄漏证明；宿主测试通过也不替代 Android 真机或性能验证。
