<p align="center"><img src="docs/brand/MeguvizMemory.svg" width="96" height="96" alt="MeguvizMemory"></p>

# MeguvizMemory

MeguvizMemory 为 CPU 缓冲区提供显式预算、读写借用（lease）和可控关闭流程（当前版本 `1.2.2`，C ABI `1.1`）。例如，解码器可以在提交后台任务前借出一帧，即使随后释放自己的句柄，后台任务仍可安全读完这一帧。

> **AI 辅助接入与迁移**：可使用项目自带 Skill [MeguvizMemory 接入与迁移](.agents/skills/meguviz-memory/SKILL.md) 盘点现有分配点、选择模块与生命周期对齐策略，并生成验证清单。

## 选择内存域

| 使用场景 | 方式 |
| --- | --- |
| 缓冲区分别在不同时间用完 | **System**。每块内存在最后一个持有者归还后单独释放。 |
| 临时对象随一批任务一起用完 | **Arena**。借用归还后，C/C++ 可在批末复位并复用内存块。Android 托管域可由整批共同持有并在批末关闭。 |
| 数据已经由解码器或其他库持有 | **导入外部内存**。由来源保证地址稳定，多个子范围可以共享一次导入。 |

同一块内存允许多个读者同时持有读借用，写借用则必须独占。域预算只统计内存域管理的底层存储和进行中的分配预留；进程 RSS、Java 对象与显存需单独观测。[外部内存约定](docs/EXTERNAL_IMPORT.md) · [C++ 借用与关闭](docs/CPP_CONVENIENCE.md)

## 快速接入

### C 和 C++

使用 CMake 3.25 或更高版本、C 编译器与支持 C++17 的工具链。在仓库根目录构建并安装：

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release
cmake --install build/core --config Release --prefix "$PWD/install/host"
```

在消费方工程中将 `CMAKE_PREFIX_PATH` 指向安装目录，再链接 `MeguvizMemory::mgmem`：

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMemory::mgmem)
```

C 接口使用 `<mgmem/mgmem.h>`，C++17 封装使用 `<mgmem/memory.hpp>`。[原生接入与图像示例](docs/INTEGRATION.md)

### Android

Java 和 Kotlin 可直接通过 `ManagedMemoryDomain` 管理申请和借用；包含 C/C++ 代码的应用可通过 AAR 内的 Prefab 模块或独立 NDK SDK 接入。`mgmem-android` 版本为 `1.2.2`（不属于 `meguviz-bom:1.2.0`），组装 AAR、本地发布与 ABI 说明见 [Android 接入指南](docs/ANDROID_QUICKSTART.md) 与 [Android 打包说明](bindings/android/README.md)。

## 文档

- [Android 内存域的关闭顺序](docs/ANDROID_MANAGED_DOMAIN.md)
- [在 Java 中借用原生内存](docs/ANDROID_NATIVE_BORROW.md)
- [与 Compute 交换只读映射](adapters/compute/README.md) · [与 Media 交换帧](adapters/media/README.md)
- [在 Compute 中使用 Memory 缓冲区](adapters/compute_host/README.md) · [Android AHB 的 CPU 映射](adapters/compute_ahb/README.md)
- [GPU-only AHB 写入协调](adapters/compute_gpu_ahb/README.md) · [GLES AHB 上传](adapters/gpu_ahb_gles/README.md)
- [已有模块的迁移与评估指南](docs/AUTOMATED_ADOPTION.md)

## 许可

项目采用 [Apache-2.0](LICENSE)，其他声明见 [NOTICE](NOTICE)。
