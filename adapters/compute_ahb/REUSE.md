# 复用 AHardwareBuffer 映射槽

连续映射不同内容时，`<mgmem/ahb_slot.h>` 的映射槽可复用桥自身的小状态对象。它不会复用 AHB 的锁或 Compute 预约：每次映射仍取得对应的完整内容 lease、等待 ready fence，最后一个 Memory 借用归还后同步解锁并交还预约。旧的 `mgmem_ahb_map` 和 `mgmem::ahb::read` 保持可用。

```cpp
// 由调用方持久保存，直到恢复成功或隔离处理完成。
void preserve_slot(mgmem_ahb_slot *, mgmem_ahb_recovery *);

mgmem_ahb_slot *slot = nullptr;
if (mgmem_ahb_slot_create(&slot) != MGMEM_OK) return;
auto mapped = mgmem::ahb::read(slot, memory, lease, external_id, 100, on_error, context);
if (mapped.status == MGMEM_OK) {
    // 可按普通 Memory 规则借用 mapped.owner；所有借用结束后归还 owner。
    if (mgmem_release_v1(memory, mapped.owner) != MGMEM_OK) {
        preserve_slot(slot, nullptr);
        return;
    }
} else if (mapped.recovery) {
    preserve_slot(slot, mapped.recovery);
    return;
}
// BUSY 表示 owner、借用、恢复句柄仍在使用，或释放状态不确定。
// 必须保留 slot、Memory domain 和回调上下文，不能强制销毁。
if (mgmem_ahb_slot_close(&slot) != MGMEM_OK) preserve_slot(slot, nullptr);
```

C 使用 `mgmem_ahb_slot_read` / `mgmem_ahb_slot_write`，C++ 使用 `mgmem::ahb::read(slot, ...)` / `write(slot, ...)`。写入要求与原写桥一致，AHB 同时具有 CPU READ 和 WRITE 用途，且 Compute lease 为 WRITE。

一个槽同一时间只能承载一个映射。调用方必须串行化槽的创建、映射和关闭；Memory 最后借用可由其他线程归还。归还前关闭、再次映射会返回 BUSY。关闭成功可能早于其他线程的最终释放调用返回；销毁 Memory domain、回调上下文或卸载桥代码前，仍须等待该释放调用返回。导入前失败若返回 recovery，必须先成功恢复才能复用；fence 失败或无法确认时不能恢复，应保留 recovery 与槽并隔离。最终 checked release 无法确认时槽也会保持 BUSY 并隔离，不能再使用或销毁。调用方须使槽、Memory domain、错误回调及上下文活到最终归还或隔离状态处理完成。所有跨运行库物理别名、布局、超时与 fence 限制与原 AHB 桥相同。
