<p align="center"><img src="docs/brand/MeguvizMemory.svg" width="96" height="96" alt=""></p>

# MeguvizMemory

MeguvizMemory 给一组 CPU 缓冲区设定共同的预算，并管理它们的生命周期。例如，解码器把一帧图像交给后台任务时，可以先取得读借用，再提交任务。即使解码器随后释放自己的句柄，后台任务仍能读完这一帧。

核心提供 C 接口和 C++17 封装。Android 应用可以通过 Java 或 Kotlin 使用托管内存域。只有通过域申请或导入的内存才计入预算，应用的其他分配仍由原分配器管理。

[原生项目接入](docs/INTEGRATION.md) · [Android 快速接入](docs/ANDROID_QUICKSTART.md) · [评估已有模块](docs/AUTOMATED_ADOPTION.md)

## 构建和安装

从仓库根目录执行以下命令。需要 CMake 3.25 或更高版本、C 编译器和支持 C++17 的 C++ 编译器。Windows 可在 Visual Studio 开发者 PowerShell 中运行。

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
cmake --install build/core --config Release --prefix "$PWD/install/host"
```

配置应用时，将 `CMAKE_PREFIX_PATH` 指向上一步得到的 `install/host`。在应用的 `CMakeLists.txt` 中链接自己的目标（示例中为 `my_app`）。

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MeguvizMemory::mgmem)
```

C 使用 `<mgmem/mgmem.h>`，C++ 使用 `<mgmem/memory.hpp>`。仓库自带一个读取 PPM、输出灰度 PGM 的小程序，可用来确认本机安装。[运行图像示例](docs/INTEGRATION.md#运行图像示例)

## 选择内存域

| 你的数据怎样使用 | 适用方式 |
|---|---|
| 单个缓冲区较大，或各块数据在不同时间用完 | 选 **System**。每块分配在最后一个持有者归还后单独释放。 |
| 一批临时对象一起用完 | 选 **Arena**。C/C++ 可在批末复位并把内存块留给下一批。Java 托管域可由整个批次共同持有，在批末关闭。 |
| 数据已经由解码器或其他库持有，且来源能保证地址稳定 | **导入外部内存**。由来源保留和释放底层存储，多个子范围共用一次导入。[导入约定](docs/EXTERNAL_IMPORT.md) |

预算包括域已持有的底层存储和正在进行的分配预留。Arena 即使归还了批内每个对象，仍会保留内存块供下次使用。进程 RSS、Java 对象和 GPU 显存需分别观测。

## 所有权与关闭

分配返回一个持有者（owner）。每次访问由一份借用（lease）限定字节范围和读写权限。同一块内存允许多个读者，写者需要独占。归还 owner 后不能再借出，但已经取得的 lease 可以用到任务结束。异步提交失败或任务取消时，也要归还提前取得的 lease。

- **C++** 的同步临时缓冲区用 `domain.with_allocation`，回调退出时归还。需要跨线程访问时显式持有 lease。最后一个持有者可能在当前线程同步回收内存。[C++ 用法与完整示例](docs/CPP_CONVENIENCE.md)
- **C** 先调用 `mgmem_domain_begin_close_v1` 停止新申请和借用，等已有任务归还资源，再由清理线程执行 `collect`。`drain` 用于等待关闭完成。[原生关闭顺序](docs/INTEGRATION.md#关闭内存域)
- **Android** 的 Java 调用方可用 try-with-resources 归还 allocation 和 lease。域的 `close()` 启动后台清理，观察到 `CLOSED` 后调用 `forgetClosed()` 归还观察令牌。[托管域生命周期](docs/ANDROID_MANAGED_DOMAIN.md)

## 其他接入方式

| 要处理的数据 | 适配器 |
|---|---|
| 在 CPU 上读取 Compute 缓冲区或 Media 帧 | [导入 Compute 只读映射](adapters/compute/README.md) · [导入 Media 帧](adapters/media/README.md) |
| 把 Memory 数据交给 Compute | [创建 Compute 宿主资源](adapters/compute_host/README.md) |
| 访问 Android 硬件缓冲区 | [CPU 映射](adapters/compute_ahb/README.md) · [交给已有 GPU 执行器写入](adapters/compute_gpu_ahb/README.md) · [用 GLES 上传 RGBA8 图像](adapters/gpu_ahb_gles/README.md) |

评估已有模块时，从分配位置、实际大小和最后一次访问入手。仓库提供离线评估工具，以及可在支持项目技能的编辑器中调用的 `$migrate-to-meguvizmemory`。[迁移指南](docs/AUTOMATED_ADOPTION.md) · [项目技能](.agents/skills/migrate-to-meguvizmemory/SKILL.md) · [性能测量与结果](docs/PERFORMANCE_PATHS.md)

源码版本为 **1.2.1 工程预览**，核心原生 ABI 为 **1.1**。项目采用 Apache-2.0 许可证。[版本与验证范围](STATE.md) · [LICENSE](LICENSE) · [NOTICE](NOTICE)
