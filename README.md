<div align="center">

<img src="docs/brand/MeguvizMemory.svg" width="96" height="96" alt="MeguvizMemory">

# MeguvizMemory

**显式 CPU 内存域、有界 arena 与所有权 / 借用合同库**

System provider · mgmem ABI · 受控字节 lease

<br>

[![Language](https://img.shields.io/badge/lang-Python-DEA584?style=flat-square&labelColor=2d3a55)](https://github.com/Meguvizarchic/MeguvizMemory)
[![Issues](https://img.shields.io/github/issues/Meguvizarchic/MeguvizMemory?style=flat-square&labelColor=2d3a55)](https://github.com/Meguvizarchic/MeguvizMemory/issues)

[构建和安装](#构建和安装) · [所有权与关闭](#所有权与关闭)

</div>

---

<details>
<summary><b>目录</b></summary>

- [构建和安装](#构建和安装)
- [所有权与关闭](#所有权与关闭)

</details>

---

当前开发接续与快路径说明见 [性能路径与接入](docs/PERFORMANCE_PATHS.md)，各项实测范围见 [STATE.md](STATE.md)。

显式 CPU 内存域、System provider、有界 arena 和可验证的所有权/借用合同。当前版本为 **0.2.0 功能工程预览**，核心原生接口使用 `mgmem_*_v1` / ABI 1.1，压力适配另提供可选的 v2 接口。独立于 BIL、Xposed、GPU 后端及 Android UI；不接管进程全局 allocator。

继续开发：新增可选的外部释放状态检查、Java 受控字节 lease，以及独立的 Compute CPU 只读桥；宿主功能检查通过，新增 Android 运行、R8 和性能资格仍待验证。见[开发状态](STATE.md)。

后续源码还补充了 C++ checked 导入、Java 关闭状态与独立 Java 图像消费者；此前预览包不包含这些改动。[当前证据](artifacts/continuation-next-20261101/README.md)。

最新源码提供可选 Java 托管 System/Arena 域及同域压力绑定，并修复控制器创建与并发关闭的竞争；宿主检查通过。已从 `2e25092` 快照生成新的三 ABI Android 和 Windows 宿主工程候选；Android 运行验证仍待完成。[托管域合同](docs/ANDROID_MANAGED_DOMAIN.md)、[压力合同](docs/ANDROID_MANAGED_PRESSURE_CONTRACT.md)、[候选证据](artifacts/delivery-20261101/final-candidates-2e25092.json)。

新版还提供 [Android 后台使用示例](consumers/android_managed/README.md)和[多域 Arena 功能诊断](bench/TLB_MULTI_ARENA.md)；宿主或源码编译通过不代表 Android 运行、硬件机制或性能资格。

首个0.2功能阶段收尾：packed reader、外部CPU内存安全导入、Android压力与自身RSS/PSS观测适配已实现。Host Release/ASan各10项、Java packed32/真实oracle288、Android原生9/ART32/压力20功能通过；家族仅合同审计，BIL不接入，性能与正式发布仍未资格。见[阶段收尾](docs/FEATURE_STAGE_20261030.md)。
上一消费者阶段（closefix1）完成DEX外部导入、图像压力/预算/异步关闭及原生900秒循环；当时JNI可重入锁修复后的API33 Android长稳未获资格。旧阶段结果见[交付与阻塞](docs/CONSUMER_CLOSEOUT_20261030.md)，不覆盖下述新版验证。
最新Android关闭阶段：本机停滞复现时观察到cgroup冻结；JNI自动通知重试与错误路径已修复。最终脱敏安装版API33 arm64真机15分钟采样无冻结、关闭归零，API34 x86_64模拟器短测通过；这是限定设备与工作量的功能/稳定性证据。见[关闭调查](docs/ANDROID_CLOSE_INVESTIGATION_20261030.md)。
提供 CMake 安装包、C ABI 与可选 C++17 move-only RAII 适配。没有新增第三方生产依赖。

本批交付范围与证据见 [STATE.md](STATE.md)、[验证记录](docs/VERIFICATION.md) 和 [TASKS.json](TASKS.json)。功能验证、设备运行、性能收益、TLB 机制和发布资格分别记录；研究模型中的 3.55% 等数值不代表本库实测。

源码锁及历史采集记录按原始字节校验；仓库检出时保留 Git 中的字节，不应为统一换行而重新归一化历史文件。

已完成 API 37、arm64、4 KB 真机的核心/C/C++ 安全用例；独立真实 DEX A1/A3 比较完成 32 块、128 次任务，性能为 INCONCLUSIVE，当前不接入 BIL。点估计与探索性区间不构成稳定收益，详见 [性能报告](docs/PERFORMANCE_A1_A3.md) 和 [接入决定](docs/BIL_ADOPTION_DECISION.md)。

DEX直接借用/复制四组、图像full/三行环持久工作区和真实v2采集器均已实现并在新arm64设备执行。core3与196消费者功能用例通过；pilot128任务完成，DEX正式4/12窗口后环境停止，图像因pilot精度不足未进入正式轮。未取得稳定性能或资源净收益，见[v2最终结果](docs/DEVICE_V2_RESULTS.md)。普通借用/布局资源优化与Memory增量分开；自身RSS/PSS已有观测能力，同完成量资源收益及能量仍未验证。

## 构建和安装

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install/host"
cmake --build build/core --config Release --parallel 2
ctest --test-dir build/core -C Release --output-on-failure
cmake --install build/core --config Release
```

Windows 使用已初始化的 Visual Studio C++ 开发环境。当前机器也可用 `tools/host-build.cmd` 包装上述 CMake 参数；`MGMEM_VS_ROOT` 可覆盖 Visual Studio 路径。地址检测构建用 `-DMGMEM_SANITIZE=ON`，MSVC 运行时须能找到其 `clang_rt.asan_dynamic-x86_64.dll`。`BUILD_SHARED_LIBS=OFF` 提供 PIC 静态核心，家族组合仍须遵守单一最终 runtime 合同。

消费者独立配置，只使用安装导出目标：

```cmake
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(my_consumer PRIVATE MeguvizMemory::mgmem)
```

```sh
cmake -S consumers/image -B build/image -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install/host"
cmake --build build/image --config Release
# P6 RGB → 整数灰度 → clamp 边缘 3×3 box blur → P5 PGM
build/image/mgmem_image input.ppm output.pgm
# System 对照使用 --system；两种模式工作量与输出相同。
```

Windows 运行消费者前将 `install/host/bin` 加入当前进程 PATH；Unix 按安装位置配置动态库搜索路径。完整 DexMeguviz 消费者需要真实外部源码和 DEX 输入，见 [集成说明](docs/INTEGRATION.md)，未捆绑或伪造 Dex SDK。

## 所有权与关闭

`allocate` 返回 owner handle；访问前获取读/写 lease。读 lease 可共享，写 lease 排斥其他 lease。owner release 后已有 lease 保活 backing，最后一次访问结束再归还 lease。异步任务必须先取得 lease 再入队。

```text
OPEN → begin_close → CLOSING
  → 完成并 release 全部 owner/lease
  → cleanup worker 调用 collect（同步 provider 释放）
  → CLOSED → drain 等待确认 → destroy
```

`begin_close` 不等待任务、不运行 provider 回调；`drain(0)` 只轮询，超时不强制释放。`collect`、`trim` 及最后一次 System release 可能同步调用 provider，需安排在允许阻塞的线程。C++ RAII 的最后一个 wrapper 同样负责同步 cleanup。裸指针、裸 domain 指针及运行库卸载都有外部寿命要求，完整合同见 [ADR](docs/ADR-001-ownership-and-abi.md) 和头文件。

预算约束唯一managed backing（owned和imported）与pending reservation，不包含allocator/容器元数据或RSS/PSS。arena的retained含reset前不能复用的已释放空间，不能直接称为RSS节省。Android压力适配JNI已有；通用ByteBuffer借用、slab、page hint、TLB placement和GPU桥未选择。

本仓库保留实现与可审查进度；没有发布正式 Release 或宣称 BIL 性能获益。原始研究附件留在本地 `research/`，公开文档仅记录其 hash、合同摘要及当前实现状态。

最新v2：新设备API33/arm64/4KB的core3与196消费者用例通过；pilot128任务完成，图像因精度不足停止formal，DEX formal64次成功后因环境门槛中止。正式性能未获资格，BIL保持原实现。详见[v2结果与区间纠正](docs/DEVICE_V2_RESULTS.md)。
