# C++ 可检查外部导入

`mgmem::domain::import_external_checked` 是头文件形式的可选 C++17 适配，接收 `mgmem_external_buffer_checked_v1` 并返回现有 move-only `mgmem::owned_allocation`。它沿用完整外部 region、origin retain/release、单一 runtime、只读/写 lease 和软预算合同；不会复制存储或跨 ABI 暴露 C++ 容器。该方法直接链接 `mgmem_import_external_checked_v1`：调用程序必须确保部署的同一个 Memory 运行库导出该符号且具有对应 capability，不能仅凭 ABI 1.1 或 1.2.1 判断可用。旧运行库在加载时就可能因缺符号失败；需要动态回退时由集成方先解析同一运行库的符号并核对 capability。

## 释放状态与隔离

- 显式 `owned_allocation::release()` / `borrow::release()` 返回 `MGMEM_OK` 时，wrapper 被消费；多次 release 空 wrapper 幂等返回 OK。
- checked owner 或由它取得的借用在 release 返回 `MGMEM_PROVIDER_ERROR` 时，C++ 层无法证明底层句柄是否已被消费，也不能确定外部回调是否已经发生。它立即将 wrapper 置空、**不重试该句柄**，并将共享 domain 置为粘性 quarantine。该状态不等于释放成功，也不承诺原始回调没有发生。其他返回错误沿用旧合同：wrapper 保留，调用方可检查状态。
- `domain::observe()` 一次顺序采样域快照、导入快照与粘性隔离标志（非原子，标志最后采样），隔离后仍可用且不置标志；`domain::quarantined()` 允许仍持有 domain wrapper 的调用方观察这一粘性状态。此后由 C++ wrapper 发起的新 allocate、import、borrow、set_budget、reset、trim、collect、drain 会返回 `MGMEM_PROVIDER_ERROR`；`begin_close`、快照和已存在 owner/lease 的 release 仍可调用，已受理借用不会被撤销。快照只是核心当时的观测，不能用来声称已正常归零。
- 隔离检查与后续进入 core 不是持锁事务：其他线程若在隔离标志置位之前已经通过检查，该操作可能随后完成。需要严格截止时由调用方在入队/调用前协调；适配器不会跨外部回调持锁，也不会让 begin_close 等待用户回调。
- 如 checked 导入本身返回 `MGMEM_PROVIDER_ERROR`，适配器读取 import snapshot。确认未 poison 且无 pending import 时，允许按失败原子 retain 合同继续使用域；无法读取、已 poison 或存在 pending import 时保守隔离。pending 也可能属于并发的其他导入，因此这是避免遗漏不确定 reservation 的保守误隔离，而不是证明该其他导入有错。
- 隔离域最后一个 C++ wrapper 销毁时，适配器刻意保留底层 domain、账目和不确定 claim；不自动 collect、destroy、重试回调或假称关闭成功。析构或移动赋值清理旧值时都无法返回状态：需要直接获知结果的调用方应先显式 release，并保留相应 domain wrapper 检查状态。丢弃全部 wrapper 后不能通过本适配器找回该域；origin context、回调代码和运行库必须仍保持有效，直至进程结束或有另行定义的安全恢复机制。当前没有强制回收或恢复接口。

未使用 checked 导入的原有 C++ wrapper 行为保持不变；其析构器仍要求清理成功，对未处理的合同违约调用 `std::terminate`。checked 导入的 `MGMEM_PROVIDER_ERROR` 是特例；其他非成功结果仍不得靠析构器忽略。此扩展不改变 Memory 的 C ABI 布局、旧结构和旧接口。

## 开发检查范围

`tests/cpp_checked/` 是独立宿主开发测试：使用真实 core 覆盖正常释放、失败原子 retain、owner/最后 lease 失败、析构、关闭竞争和并发 wrapper；使用 fake ABI 模拟 `MGMEM_PROVIDER_ERROR` 发生前句柄仍未消费、快照不可用以及 pending import 的情况。真实失败用例保留不确定资源并在独立进程退出，不算正常释放或无泄漏证明。宿主检查不代表 ABI 各平台、Android、设备、产品或性能资格。
