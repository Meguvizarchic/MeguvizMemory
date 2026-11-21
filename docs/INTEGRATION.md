# 在 C 或 C++ 项目中使用 MeguvizMemory

先按 [快速接入说明](../README.md#c-和-c) 安装核心库（版本 `1.2.2`）。配置消费方工程时，将 `CMAKE_PREFIX_PATH` 设为安装前缀并链接 `MeguvizMemory::mgmem`。该目标会自动向 C++ 调用方传递 C++17 要求、头文件路径以及共享库所需的编译定义：

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMemory::mgmem)
```

默认构建安装共享库。若希望将核心静态链接进单一可执行文件，可在构建核心时加上 `-DBUILD_SHARED_LIBS=OFF`。需要在多个动态库之间传递句柄的程序必须共用同一份运行库；句柄仅在其来源运行库的本次加载期间有效。

## 运行图像示例

示例程序从磁盘读取 8 位 P6 RGB 图像，转为灰度后执行 3×3 均值模糊，再输出 P5 PGM。在仓库根目录运行以下命令（`CMAKE_PREFIX_PATH` 指向已安装的核心目录）：

```sh
cmake -S consumers/image -B build/image -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$PWD/install/host"
cmake --build build/image --config Release --target mgmem_image
```

Linux 单配置生成器可直接用仓库自带小图验证输出：

```sh
build/image/mgmem_image artifacts/verification-20261030/consumers/image-17x13.ppm build/image/result.pgm
```

Windows 使用 Visual Studio 多配置生成器时，在同一 PowerShell 会话中将共享库目录加入 `PATH` 后运行：

```powershell
$env:PATH = "$PWD/install/host/bin;$env:PATH"
.\build\image\Release\mgmem_image.exe artifacts/verification-20261030/consumers/image-17x13.ppm build/image/result.pgm
```

输出图像宽 17、高 13。程序默认使用 Arena 模式，在末尾加 `--system` 可用相同输入运行 System 模式。移动过共享库位置时，请同步更新动态库搜索路径。

DEX 示例另需指定真实的 DexMeguviz 源码目录（`DEXMEGUVIZ_SOURCE_DIR`）、支持 C++20 的编译器及 DEX 输入文件。`mgmem_dex_consumer input.dex` 默认走 System，加 `--arena` 切换为 Arena。[DEX 借用示例](DEX_BORROW_CONTRACT.md) · [导入已有 DEX 缓冲区](DEX_EXTERNAL_IMPORT.md)

## 申请和借用

创建域时指定模式、预算与可选的自定义 provider。使用 C 接口时，必须填写 `mgmem_domain_options_v1` 的 `struct_size` 与 ABI 版本字段；C++ 接口 `create_system` 和 `create_arena` 会自动填充这些字段。[C 头文件](../include/mgmem/mgmem.h) · [C++ 示例](CPP_CONVENIENCE.md)

1. 调用 `mgmem_allocate_v1` 申请 `owner`，字节数须大于零，对齐值须为 2 的幂。原生分配的内存**未初始化**，须由写入方初始化。
2. 调用 `mgmem_borrow_acquire_v1` 借出一段范围。`MGMEM_READ` 允许多个读者共存，`MGMEM_WRITE` 排斥同一 `allocation` 上的其他借用。需要异步排队的任务应在入队前取得 `lease`，无论是正常完成、提交被拒还是任务取消，都由持有者负责归还。
3. 调用 `mgmem_release_v1` 归还 `owner` 后会停止新的借用，但已取得的 `lease` 仍保有有效地址，直到所有任务完成最后一次访问并调用 `mgmem_borrow_release_v1`。地址和句柄均必须交还给创建它们的运行库。

需要扩容时，先申请新块并完成初始化或复制，再交付新 `owner`，因此预算必须容得下新旧两块短暂共存。新申请失败时，原 `owner` 仍可继续使用。`backing_bytes` 统计域持有的底层存储容量（包含完整导入的外部内存），`reserved_bytes` 统计进行中的分配预留；快照中的 `live`、`retiring` 和 `retained` 均已包含在 `backing_bytes` 内。[外部内存与预算](EXTERNAL_IMPORT.md)

## 关闭内存域

直接调用 C 接口时按以下顺序关闭（Java 托管域由内置后台线程负责清理，见 [Android 关闭流程](ANDROID_MANAGED_DOMAIN.md#关闭内存域)）：

1. 停止提交新任务，调用 `mgmem_domain_begin_close_v1` 拒绝新的申请与借用请求；已持有 `lease` 的任务仍可处理完既有数据。
2. 等待持有者与进行中的任务归还 `owner` 和 `lease`。System 域最后一次释放可能会同步调用 provider 释放函数，应安排在允许阻塞的线程执行。
3. 在清理线程调用 `mgmem_domain_collect_v1`。返回 `MGMEM_BUSY` 表示仍有资源、操作或另一轮回收未结束，待其完成后重新调用；返回 `MGMEM_OK` 后域进入 `CLOSED`。
4. 其他线程可调用 `mgmem_domain_drain_v1` 等待关闭完成（超时为 `0` 时仅查询状态）。超时后不得强行销毁，应保留域让清理线程继续推进。持有 `lease` 的线程必须先归还自身的 `lease`，再等待依赖它的关闭。
5. 确认所有域调用均已返回后，由唯一最终持有者调用 `mgmem_domain_destroy_v1`；此后域指针失效。

如果希望继续复用 Arena 域，在整批 `owner` 和 `lease` 全部归还后调用 `mgmem_arena_reset_v1` 重置内部游标；若要在域处于 `OPEN` 且空闲时将底层块归还给 provider，可调用 `mgmem_domain_trim_v1`。

## 自定义 provider 与跨库使用

核心会复制 provider 函数表并保留其 `context`。provider 回调可能在不同线程上被并发调用，必须保证线程安全、不抛出异常且不重入同一域。申请成功时须输出满足容量和对齐要求的存储，失败时将 `*out` 保持为 `NULL`；释放时归还原始地址、字节数和对齐值。

当底层回收状态无法确认时，核心会隔离相关状态并保留账目，绝不重试可能已经释放过的地址。特别是最终 `destroy` 因 `provider->release` 异常返回 `MGMEM_PROVIDER_ERROR` 时，域对象本身已被消费，不可再次调用 `destroy`；因前置条件未满足返回 `MGMEM_BUSY` 时，域仍归调用方持有。[所有权与 ABI 约定](ADR-001-ownership-and-abi.md)

Compute 资源的映射、GPU 完成信号和 Media 帧各自拥有独立的生命周期。跨库组合时请使用对应适配器；包含多个动态库的应用必须在所有域、任务与回调彻底结束后再考虑卸载动态库。需要回退到 System 或原有实现时，仅切换新任务的分配方式，让旧域沿着原有释放路径自然收尾。[跨库所有权](FAMILY_MEMORY_CONTRACTS.md) · [Compute 只读映射](../adapters/compute/README.md) · [Media 帧](../adapters/media/README.md)
