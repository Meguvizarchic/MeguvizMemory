<p align="center"><img src="docs/brand/MeguvizMemory.svg" width="96" height="96" alt="MeguvizMemory"></p>

# MeguvizMemory

MeguvizMemory 为 CPU 缓冲区提供预算、读写借用和生命周期管理。例如，解码器可以在提交后台任务前借出一帧，即使随后释放自己的句柄，后台任务仍可读完这一帧。

## 选择内存域

| 使用场景 | 方式 |
| --- | --- |
| 缓冲区分别在不同时间用完 | **System**。每块内存在最后一个持有者归还后单独释放。 |
| 临时对象随一批任务一起用完 | **Arena**。借用归还后，C/C++ 可在批末复位并复用内存块。Android 托管域可由整批共同持有并在批末关闭。 |
| 数据已经由解码器或其他库持有 | **导入外部内存**。由来源保证地址稳定，多个子范围可以共享一次导入。 |

一个缓冲区允许多个读者，写借用需要独占。预算覆盖内存域持有的底层存储和分配预留。进程内存、Java 对象与显存需分别观测。[外部内存约定](docs/EXTERNAL_IMPORT.md) · [C++ 借用与关闭](docs/CPP_CONVENIENCE.md)

## 快速接入

### C 和 C++

使用 CMake 3.25 或更高版本、C 编译器与支持 C++17 的工具链。从仓库根目录构建并安装。

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release
cmake --install build/core --config Release --prefix "$PWD/install/host"
```

配置应用时让 `CMAKE_PREFIX_PATH` 指向安装目录，再链接到应用自己的目标。

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMemory::mgmem)
```

C 使用 `<mgmem/mgmem.h>`，C++ 使用 `<mgmem/memory.hpp>`。[原生接入与图像示例](docs/INTEGRATION.md)

### Android

Java 和 Kotlin 可使用 `ManagedMemoryDomain` 管理申请和借用。含 C/C++ 代码的应用可通过 Prefab 或独立 NDK SDK 接入。从源码组装 AAR 的步骤和支持的 ABI 见[Android 接入指南](docs/ANDROID_QUICKSTART.md)。

## 文档

- [Android 内存域的关闭顺序](docs/ANDROID_MANAGED_DOMAIN.md)
- [与 Compute](adapters/compute/README.md)和[Media](adapters/media/README.md)交换缓冲区
- [在 Compute 中使用 Memory 缓冲区](adapters/compute_host/README.md)以及 [Android 硬件缓冲区的 CPU 访问](adapters/compute_ahb/README.md)
- [已有模块的迁移指南](docs/AUTOMATED_ADOPTION.md)

## 许可

项目采用 [Apache-2.0](LICENSE)，其他声明见 [NOTICE](NOTICE)。
