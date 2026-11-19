# 用 C++17 管理内存域和借用

包含 `<mgmem/memory.hpp>`，链接 `MeguvizMemory::mgmem`。域、分配和借用都可移动。每份分配和借用独立持有域，因此移动或销毁外层 `domain` 对象不会让已经借出的地址提前失效。[安装和链接](INTEGRATION.md)

| 需要做什么 | 接口 |
|---|---|
| 分块申请、各自释放 | `domain::create_system(budget, out)` |
| 按批申请、整批复用 | `domain::create_arena(budget, chunk, out)` |
| 多次读写同一块数据 | `memory.allocate(bytes, owner)`，随后 `owner.with_read(body)` 或 `owner.with_write(body)` |
| 临时使用一块可写空间 | `memory.with_allocation(bytes, body)` |
| 把整块数据交给异步任务 | 提交前调用 `owner.acquire_borrow(MGMEM_READ, lease)`，把借用移动给任务 |
| 查看域状态 | `memory.observe()` 顺序读取快照、导入快照和隔离标志 |

## 完整示例

下面的程序写入一块 4 KiB 缓冲区，重新借出并逐字节校验，再申请一次临时空间。原生存储最初未初始化。示例中的 64 KiB 是演示用的 Arena 块大小，实际用量可以根据批次轨迹选择。

```cpp
#include <mgmem/memory.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

int main() {
    mgmem::domain memory;
    auto status = mgmem::domain::create_arena(1024 * 1024, 64 * 1024, memory);
    if (status != MGMEM_OK) return static_cast<int>(status);

    mgmem::owned_allocation owner;
    status = memory.allocate(4096, owner);
    if (status != MGMEM_OK) return static_cast<int>(status);
    status = owner.with_write([](void *data, uint64_t bytes) {
        std::memset(data, 0x5a, static_cast<std::size_t>(bytes));
    });
    if (status != MGMEM_OK) return static_cast<int>(status);
    status = owner.with_read([](const void *data, uint64_t bytes) -> mgmem_status {
        const auto *input = static_cast<const unsigned char *>(data);
        for (uint64_t i = 0; i < bytes; ++i)
            if (input[i] != 0x5a) return MGMEM_PROVIDER_ERROR;
        return MGMEM_OK;
    });
    if (status != MGMEM_OK) return static_cast<int>(status);

    status = memory.with_allocation(256, [](void *scratch, uint64_t bytes) {
        std::memset(scratch, 0, static_cast<std::size_t>(bytes));
    });
    return static_cast<int>(status);
}
```

创建域或分配时使用空的输出对象，检查返回的 `mgmem_status` 后再使用它。省略对齐时采用 `alignof(std::max_align_t)`。需要更高对齐时使用 `allocate(bytes, alignment, owner)` 或 `with_allocation(bytes, alignment, body)`。

## 读写范围和作用域

`with_read(body)` 和 `with_write(body)` 访问整块数据。只处理其中一段时，用 `with_read(offset, length, body)` 或 `with_write(offset, length, body)`。偏移与长度都按字节计，零长度或越界会在进入回调前返回错误。

读回调取得 `const void *`，同一块数据可有多个读借用。写回调取得 `void *`，需要独占该 allocation。回调可以返回 `void`，也可以返回 `mgmem_status` 让调用方处理业务错误。指针的使用期限止于回调返回。

偏好异常的调用方可以写 `mgmem::or_throw(memory.allocate(bytes, owner))`。失败时，`mgmem::status_error::what()` 返回已知状态名（如 `MGMEM_OUT_OF_BUDGET`），`status()` 保留原始状态值。需要在 `noexcept` 函数中处理错误时，直接检查返回值。

回调抛异常时，局部借用随栈展开释放。若使用 checked 外部导入，最后一次归还报告 `MGMEM_PROVIDER_ERROR` 时，域会进入隔离。正常返回路径上，这一清理错误优先于回调的返回值。可用 `quarantined()` 查看隔离状态，避免重试可能已被消费的外部释放。普通析构也要求底层清理成功，违反 provider 约定可能触发 `std::terminate`。要在析构前取得清理错误，可显式调用 `release()`。[Checked 导入约定](CPP_CHECKED_IMPORT.md)

## 把借用交给后台任务

提交前用空的 `mgmem::borrow` 接住借用。`owner.acquire_borrow(MGMEM_READ, lease)` 借出整块，`owner.acquire_borrow(offset, length, MGMEM_READ, lease)` 借出指定范围。写入任务可用 `MGMEM_WRITE`，并通过 `lease.write_data()` 取得可写地址。

以下代码把读借用移入任务。示例假定 `post` 接受可移动任务、返回 `mgmem_status`，并在拒绝提交或取消任务时销毁该任务。

```cpp
mgmem::borrow lease;
const auto status = owner.acquire_borrow(MGMEM_READ, lease);
if (status != MGMEM_OK) return status;

return executor.post([lease = std::move(lease)] {
    consume(lease.data(), lease.size_bytes());
});
```

任务持有借用直到最后一次访问，期间可以归还原来的 owner。这里的 lambda 只能移动，执行器的任务类型需要支持这种捕获。若取消任务时可能执行最后一次归还，把任务销毁也安排在允许同步回收的线程。

## 域的结束与复用

Arena 要等整批 owner 和 lease 归还后才可 `reset()`。它保留底层块供下一批复用。空闲时 `trim()` 可把底层块归还。正常情况下，最后一份共享所有权的析构会同步完成关闭、收集和销毁。把最后一次释放安排在允许阻塞的线程，尤其是使用自定义 provider 时。[原生关闭顺序](INTEGRATION.md#关闭内存域)

需要评估回调封装的成本时，可查阅相同读写负载下的宿主对照，结果中同时列出了中位耗时和较慢的尾部样本。[C++ 回调测量记录](../artifacts/cpp-convenience-20261109/decision.json)
