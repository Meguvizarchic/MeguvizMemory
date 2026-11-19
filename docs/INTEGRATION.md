# 在 C 或 C++ 项目中使用 MeguvizMemory

先按[构建和安装](../README.md#构建和安装)安装核心。配置使用方时，将 `CMAKE_PREFIX_PATH` 设为安装前缀。公共目标是 `MeguvizMemory::mgmem`，它会向 C++ 调用方传递 C++17 要求，也会传递头文件路径和共享库所需的编译定义。

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMemory::mgmem)
```

默认安装共享库。需要把核心静态链接进一个最终二进制时，可以在构建核心时加 `-DBUILD_SHARED_LIBS=OFF`。在多个动态库间传递句柄的程序应共用同一份运行库。句柄只在其来源运行库的本次加载期间有效。

## 运行图像示例

示例从磁盘读取 8 位 P6 RGB 图像，转成灰度，做 3×3 均值模糊，再输出 P5 PGM。在仓库根目录执行，`CMAKE_PREFIX_PATH` 指向刚安装的库。

```sh
cmake -S consumers/image -B build/image -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$PWD/install/host"
cmake --build build/image --config Release --target mgmem_image
```

Linux 使用单配置生成器时，可直接用仓库内的小图验证结果。

```sh
build/image/mgmem_image artifacts/verification-20261030/consumers/image-17x13.ppm build/image/result.pgm
```

Windows 使用 Visual Studio 生成器时，在同一个 PowerShell 窗口运行以下命令。

```powershell
$env:PATH = "$PWD/install/host/bin;$env:PATH"
.\build\image\Release\mgmem_image.exe artifacts/verification-20261030/consumers/image-17x13.ppm build/image/result.pgm
```

生成的图像宽 17、高 13。默认走 Arena，在命令末尾加 `--system` 可用相同输入运行 System。若移动了共享库，需让运行时的动态库搜索路径指向相应安装目录。

DEX 示例另需提供真实的 DexMeguviz 源码目录（`DEXMEGUVIZ_SOURCE_DIR`）、支持 C++20 的编译器及 DEX 输入。其 `mgmem_dex_consumer input.dex` 默认用 System，`--arena` 选用 Arena。[DEX 借用示例](DEX_BORROW_CONTRACT.md) · [导入已有 DEX 缓冲区](DEX_EXTERNAL_IMPORT.md)

## 申请和借用

创建域时指定模式、预算与可选的 provider。使用 C 接口时填写 `mgmem_domain_options_v1` 的结构大小和 ABI 版本。C++ 的 `create_system` 和 `create_arena` 会填写这些字段。[C 头文件](../include/mgmem/mgmem.h) · [C++ 示例](CPP_CONVENIENCE.md)

1. 用 `mgmem_allocate_v1` 申请 owner，字节数须大于零，对齐值须是 2 的幂。原生分配的内容需要由写入方初始化。
2. 用 `mgmem_borrow_acquire_v1` 借出一段范围。`MGMEM_READ` 可共享，`MGMEM_WRITE` 排斥同一 allocation 上的其他借用。需要排队的任务在提交前取得 lease，拒绝提交、取消和正常完成都要归还。
3. `mgmem_release_v1` 归还 owner，停止新的借用。已有 lease 仍保有地址，在最后一次访问后用 `mgmem_borrow_release_v1` 归还。地址和句柄都交还给创建它们的运行库。

需要扩容时，先申请新块并完成初始化或复制，再交付新 owner。预算要容得下新旧两块暂时共存。新申请失败时，原 owner 仍可继续使用。`backing_bytes` 统计域持有的底层容量，含完整导入的外部内存。`reserved_bytes` 统计在途预留。快照中的 live、retiring 和 retained 都已包含在 backing 中。[外部内存与预算](EXTERNAL_IMPORT.md)

## 关闭内存域

以下是直接使用 C 接口时的顺序。Java 托管域会安排自己的清理线程。[Android 关闭流程](ANDROID_MANAGED_DOMAIN.md#关闭内存域)

1. 停止提交新任务，调用 `mgmem_domain_begin_close_v1` 停止新申请和借用。持有 lease 的任务可以继续处理已经借出的数据。
2. 让持有者和在途任务归还 owner 与 lease。最后一次 System 释放可能同步调用 provider，需安排在允许阻塞的线程。
3. 在清理线程调用 `mgmem_domain_collect_v1`。返回 `MGMEM_BUSY` 表示仍有资源、操作或另一轮回收在途。待它们结束再收集。成功后域进入 `CLOSED`。
4. 其他线程可用 `mgmem_domain_drain_v1` 等待关闭。超时为零时只查询状态。若等待超时，应保留域，让清理线程继续工作。持有 lease 的线程须先归还它，才能等待依赖它的关闭。
5. 所有域调用都已退出后，由唯一的最终持有者调用 `mgmem_domain_destroy_v1`。此后域指针失效。

需要继续使用 Arena 时，等整批 owner 和 lease 归还后调用 `mgmem_arena_reset_v1` 复用已有块。需要归还底层容量时，在域空闲且仍开放时调用 `mgmem_domain_trim_v1`。

## 自定义 provider 与跨库使用

核心复制 provider 函数表并持有其 context。回调可能从不同线程并发调用，必须线程安全、遵守无异常约定，并避免重入同一域。申请成功时提供满足容量和对齐的存储，失败时留下空输出。归还时要使用原始 provider、地址、容量和对齐。

若回收结果未知，核心会隔离相应状态并保留未确认的记账。不可重试可能已经释放过的地址。尤其是最终 `destroy` 因 provider 的 context 释放回调抛出异常而返回 `MGMEM_PROVIDER_ERROR` 时，域已被消费。因前置条件返回 `MGMEM_BUSY` 时，域仍归调用方持有。[所有权与 ABI 约定](ADR-001-ownership-and-abi.md)

Compute 资源的映射、GPU 完成信号和 Media 帧各有自己的寿命。组合时使用对应适配器，包含多个动态库的应用应在所有域、任务和回调结束后才考虑卸载运行库。回退到 System 或应用原实现时，仅更改新任务的分配方式，让旧域按原来的释放路径结束。[跨库所有权](FAMILY_MEMORY_CONTRACTS.md) · [Compute 只读映射](../adapters/compute/README.md) · [Media 帧](../adapters/media/README.md)
