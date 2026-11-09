# C++17 常用短路径

使用 `<mgmem/memory.hpp>`，链接安装导出的 `MeguvizMemory::mgmem`。短入口复用现有
owner、lease 和域生命周期，状态仍为 `mgmem_status`。

| 场景 | 短入口 | 明确保留的选择 |
|---|---|---|
| 创建 System 域 | `domain::create_system(budget, out)` | 预算显式；可在最后传入 provider |
| 创建 Arena 域 | `domain::create_arena(budget, chunk, out)` | 预算与 chunk 大小显式；可在最后传入 provider |
| 保存 owner，多次访问 | `memory.allocate(bytes, owner)`，再 `owner.with_read(body)` / `with_write(body)` | owner 由调用方持有；读写权限分别命名 |
| 一次同步临时使用 | `memory.with_allocation(bytes, body)` | 回调结束归还本次 owner 和 WRITE lease |

创建入口完整填写当前 ABI/options 头；高级选项继续用 `domain::create(options, out)`。
域创建一次后可以反复分配和读写，短入口不会为每次操作隐式创建另一个域。

## 完整示例

以下程序展示创建域、复用 owner 和临时工作区，逐步处理失败状态。载荷最初未初始化；
示例中的 `memset` 是调用方明确要求的写入。

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

`allocate(bytes, owner)` 与省略 alignment 的 `with_allocation` 使用
`alignof(std::max_align_t)`。需要更高对齐时传入显式 alignment：
`allocate(bytes, alignment, owner)` 或 `with_allocation(bytes, alignment, body)`。
这些入口保留原 API 的预算、权限和失败状态，不隐式重试，也不自动初始化载荷。

## 读写范围和作用域

`with_read(body)` / `with_write(body)` 访问完整 owner；子范围使用
`with_read(offset, length, body)` / `with_write(offset, length, body)`。两个整数均为
字节数，越界或零长度在调用回调前返回失败。

READ 回调接收 `const void *`，可以嵌套其他 READ；WRITE 回调接收 `void *`，与
同 allocation 的其他读写借用排斥。外部只读输入仍拒绝 WRITE。回调返回 `void`
表示本体正常完成，也可返回 `mgmem_status`；其他返回类型会在编译时被拒绝。
回调自身返回的状态原样传出，不据此隔离域；quarantine 依据原生 checked 清理的实际结果。

指针只在同步回调内有效，不能返回、缓存或交给晚于回调完成的任务。异步工作继续
使用显式 `acquire_borrow`，入队前取得 lease，最后一次访问后归还。回调期间移动或
归还 owner、移动 domain wrapper 时，本地 lease 仍保活 native 域和载荷。

`with_read` / `with_write` 正常结束时明确归还 lease。若 checked 外部输入的最后一次
lease 归还失败，其错误优先于回调状态返回，域进入 quarantine；可通过原 domain
wrapper 的 `quarantined()` 观察，后续准入返回 `MGMEM_PROVIDER_ERROR`。这不代表
未知的外部释放可以重试。

回调异常沿原 RAII 合同清理并继续传播，checked 清理失败仍可留下可观察的
quarantine。普通清理失败的既有 fail-stop 合同保持；未消费的释放错误立即终止，
不会为了完成短调用再重放一次释放。

## 域的结束与复用

短调用不会隐式 reset 或 close 整个域。Arena 的已释放空间在显式 `reset()` 后复用，
`trim()` / collect 按原合同归还 backing。未进入 quarantine 的最后一份共享所有权
归还时，现有 C++ RAII 会同步执行关闭、collect、drain 和 destroy；这可能调用 provider，应在允许
同步清理的线程结束最后一次所有权。需要观测关闭状态时，仍使用原显式关闭接口。

功能、构建与性能验证分别记录；便捷入口不自动取得新的性能资格。

## 本轮验证与性能范围

本轮核心和 C ABI 头逐字节保持 `7f076e5` 的版本，只改可选 C++ 层及测试。
System/Arena 的创建、默认对齐、whole/subrange 读写、异常、提前归还及移动保活，
均有实际回归。最终核心/JNI 48/48、C++ 15/15、ASan 48/48、C++ ASan 15/15、
安装版消费者 1/1（14 个 RAII case）通过；两个错误回调类型按预期编译拒绝。

Windows MSVC Release 同一程序、相同载荷与存活集合下，三版各 192 个进程，
另各 24 个真实核心计数进程，分别独立保存。首版及只展开内部函数的第二版存在
稳定双位数 P50 代价，均未采用。最终版本针对完整短借用链请求 MSVC 内联，
实际热循环没有额外直接 C++ 调用；其他编译器使用普通 inline。没有更改全局优化
开关、禁用异常或屏蔽 C4714；Release/Debug `/W4 /WX` 的递归、异常及函数指针
兼容控制均编译通过。内联请求仍受[编译器约束](https://learn.microsoft.com/en-us/cpp/cpp/inline-functions-cpp?view=msvc-170)。

最终八组已有 owner 读写的完整 wall P50 配对比为 **0.97175–1.02113**，差中位
约 **−6.15 到 +7.91 ns/op**。两侧原生调用、provider 事件、有效创建字段、
当前 EXE 绑定范围的 C++ 上游申请、校验和与归零资源逐 round 相同。
这不等于跨平台或任意回调都没有时间成本。

保留的明显尾部反向：System whole READ 的完整 wall P95 **1.12876**，约
**+28.81 ns/op**；Arena subrange WRITE 的 P95 **1.10359**，约 **+17.82 ns/op**，
均为 6/8 对慢，前后半及 AB/BA 同样保留反向，不能归为噪声。两个 whole WRITE
的 P50 也约为 **1.02113/1.02019**。诊断 EXE 的 `.text` 相对首版增加 656 B，
不是所有客户端的固定体积成本。全部正反向、源码和生成代码分别归档。

Android 只新编链消费者 1 ELF、编译 3 REL TU，核验复用的原核心 SO；没有执行
目标。以上不构成 Android 运行、RSS、能耗或正式产品性能资格。
[完整采用决定、三轮对照与全部反向](../artifacts/cpp-convenience-20261109/decision.json)。
