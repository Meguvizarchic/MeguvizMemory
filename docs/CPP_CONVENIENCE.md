# 用 C++17 管理内存域和借用

包含 `<mgmem/memory.hpp>`，链接 `MeguvizMemory::mgmem`。`domain`、`owned_allocation` 和 `borrow` 均为只可移动（move-only）的轻量包装；每次分配和每次借用都独立持有底层域引用，因此移动或销毁外层 `domain` 对象不会导致存活中的地址提前失效。[安装和链接](INTEGRATION.md)

| 需要做什么 | 接口 |
|---|---|
| 分块申请、各自释放 | `domain::create_system(budget, out)` |
| 按批申请、整批复用 | `domain::create_arena(budget, chunk, out)` |
| 多次读写同一块数据 | `memory.allocate(bytes, owner)`，随后 `owner.with_read(body)` 或 `owner.with_write(body)` |
| 临时使用一块可写空间 | `memory.with_allocation(bytes, body)` |
| 把整块数据交给异步任务 | 提交前调用 `owner.acquire_borrow(MGMEM_READ, lease)`，把借用移动给任务 |
| 查看域状态 | `memory.observe()` 顺序读取快照、导入快照和隔离标志 |

## 完整示例

下面的程序向 Arena 域申请 4 KiB 缓冲区、写入模式字节并用读借用校验，最后再使用一次同步临时空间。原生存储初始未初始化；示例中的 64 KiB 仅用于演示，实际 `chunk` 尺寸应依据批次轨迹确定。

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

创建域或分配时须传入空的输出对象，检查返回的 `mgmem_status` 为 `MGMEM_OK` 后再使用。省略对齐参数时默认使用 `alignof(std::max_align_t)`；需要自定义对齐时改用 `allocate(bytes, alignment, owner)` 或 `with_allocation(bytes, alignment, body)`。

## 读写范围和作用域

`with_read(body)` 和 `with_write(body)` 访问整块数据；若只需访问子区间，使用 `with_read(offset, length, body)` 或 `with_write(offset, length, body)`。偏移与长度均以字节为单位，零长度或越界会在进入回调前返回错误。

读回调接收 `const void *`，同一份分配允许多个读借用并发存在；写回调接收 `void *`，会排斥同一 `allocation` 上的其他借用。回调可返回 `void` 或 `mgmem_status`，但传入的指针在回调返回后即告失效。

若项目采用异常流处理错误，可配合 `mgmem::or_throw(memory.allocate(bytes, owner))` 使用；失败时抛出的 `mgmem::status_error::what()` 会返回状态名（例如 `MGMEM_OUT_OF_BUDGET`），`status()` 保留原始错误码。在 `noexcept` 路径下应直接检查 `mgmem_status` 返回值。

当回调抛出异常时，局部 `borrow` 会随栈展开释放。若涉及 checked 外部导入且最终归还返回 `MGMEM_PROVIDER_ERROR`，域会进入隔离态，正常路径上清理错误会覆盖回调返回值，可通过 `quarantined()` 查询隔离状态（此时不得重试外部释放）。普通析构也要求底层清理成功，违约可能触发 `std::terminate`；需要显式捕获清理错误时请先调用 `release()`。[Checked 导入约定](CPP_CHECKED_IMPORT.md)

## 把借用交给后台任务

提交任务前准备空的 `mgmem::borrow`，调用 `owner.acquire_borrow(MGMEM_READ, lease)` 借出整块（或指定 `offset` 和 `length`）。写入任务可传入 `MGMEM_WRITE`，并通过 `lease.write_data()` 获取可写地址。

以下代码把读借用移入后台任务（假定 `executor.post` 接收 move-only 任务并返回 `mgmem_status`，且在拒绝提交或取消时销毁任务对象）：

```cpp
mgmem::borrow lease;
const auto status = owner.acquire_borrow(MGMEM_READ, lease);
if (status != MGMEM_OK) return status;

return executor.post([lease = std::move(lease)] {
    consume(lease.data(), lease.size_bytes());
});
```

任务对象持有 `lease` 直至最后一次访问结束，主控方在此期间即可释放原有的 `owner`。由于异步任务捕获了只可移动的 `borrow`，当任务取消可能执行最后一次释放时，应将任务析构也安排在允许阻塞的清理线程。

## 域的结束与复用

Arena 域需等待整批 `owner` 和 `lease` 归还后方可调用 `reset()`，之后底层块会被留作下一批复用；域空闲时调用 `trim()` 则把底层块归还给 provider。最后一份共享状态销毁时会自动顺序执行 `begin_close`、`collect`、`drain` 与 `destroy`，因此请确保最后一个包装对象在允许阻塞的工作线程上析构（使用自定义 provider 时尤其关键）。[原生关闭顺序](INTEGRATION.md#关闭内存域)

## 适用范围与验证说明

如需评估 C++ 回调封装相比裸接口的额外开销，可查阅同一读写负载下的宿主测量记录（包含中位耗时与尾部样本）：[C++ 回调测量记录](../artifacts/cpp-convenience-20261109/decision.json)。该数据仅反映宿主对照结果，不构成跨平台或设备端性能保证。
