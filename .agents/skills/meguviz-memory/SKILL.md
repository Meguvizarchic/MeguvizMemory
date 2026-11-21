---
name: meguviz-memory
description: 为 C/C++17 与 Android（Java/Kotlin/JNI）工程接入 MeguvizMemory 1.2.2，或将存量 malloc、new、裸缓冲区及数组转传迁移为带软预算、读写租约（lease）和可控关闭流程的内存域管理。在需要限制 CPU 缓冲区总容量、跨线程保活借用、为批次临时块引入 Arena 复用、零复制导入外部稳定缓冲区、使用 Android ManagedMemoryDomain / NativeMemoryLease（copyTo、withDirectBuffer）或对接 Compute/Media/AHB 交换桥时触发。涉及 GPU 管线与 Vulkan/GLES 计算调度时转交 MeguvizCompute；涉及音视频解码与帧时间线管理时转交 MeguvizMedia。
---

# MeguvizMemory 接入与迁移

为 CPU 缓冲区提供显式软预算、读写互斥借用与可验证关闭流程，版本 `1.2.2`（C ABI `1.1`）。

## 第 1 阶段：盘点工程现状

修改代码前，先只读排查目标工程并向用户输出一份简短评估（目标域类型、预算与 chunk 假设、外部 ID 与释放线程边界、回退方式）：

1. **核对构建基线**：
   - C/C++ 工程检查 `CMakeLists.txt`：要求 CMake `3.25+`、C11 / C++17（如 DEX 示例需 C++20）及与上游 SDK 一致的 CRT/STL。
   - Android 工程检查 `build.gradle.kts` / `libs.versions.toml`：`minSdk 27`、`compileSdk 37.2`、JDK `17`、目标 ABI（`arm64-v8a`、`armeabi-v7a`、`x86_64`），以及是否开启 `buildFeatures.prefab = true`。
2. **定位缓冲分配与交付点**：
   - 沿每个分配点记录：申请方、最后读写方、跨线程或异常/取消时由谁释放、地址是否要求稳定、是否已有复用池或属 GPU/栈上资源。
   - 已有 Memory 调用或待排查目录，可在 MeguvizMemory 仓库根目录运行扫描与评估脚本（工具只生成评审报告，不切换运行时路径）：
     - `python tools/draft_inventory.py --root <目标目录> --output inventory.draft.json`
     - `python tools/run_adoption.py --inventory sites.json --trace <site_id>=trace.jsonl --out-dir build/adoption-review`
   - 核对输出 `classification.json`、`receipt.json` 与 `shadow-<site>.json`：只有静态分类为 `arena_candidate`、报告结论为 `arena_recommended`、SHA-256 与策略匹配且预算覆盖至少一个 `chunk` 时才采用 Arena；已有优化池、栈对象或 GPU 托管资源保持原实现。

## 第 2 阶段：选择模块与接入依赖

按实际场景选择最小依赖，严禁将未使用的可选适配器拉入工程。

| 场景 | 推荐模块 / 坐标 / CMake Target | 说明与边界 |
|---|---|---|
| C / C++ 核心（System / Arena / 外部导入） | `find_package(MeguvizMemory CONFIG REQUIRED)`<br>链接 `MeguvizMemory::mgmem` | C 用 `<mgmem/mgmem.h>`，C++17 用 `<mgmem/memory.hpp>`；跨动态库传递句柄须共用同一份共享库 |
| C 原生压力控制器（可选） | `MeguvizMemory::pressure` | `<mgmem/pressure.h>`，v1/v2 软预算调压与后台静默 `trim` |
| Android Java / Kotlin 托管域与借用 | `io.github.jichuo1.meguviz:mgmem-android:1.2.2`<br>（本地 Maven 仓库或 `mgmem-android-release.aar`） | 含 `ManagedMemoryDomain`、`NativeMemoryLease`、`MemoryPressureAdapter`；**不在 `meguviz-bom:1.2.0` 内**，需由 `bindings/android` 组装或独立引用 |
| Android 原生 C/C++ + JNI 互操作 | Prefab：`find_package(mgmem-android CONFIG REQUIRED)` 链接 `mgmem-android::mgmem` / `mgmem-android::mgmem_pressure_jni`<br>或独立 SDK：`find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)` 链接 `MeguvizMemory::android_jni` | 禁止在同一 APK 中混装多份 `libmgmem.so`；Java 与 JNI 必须来自同批次构建 |
| Compute / Media / AHB / Packed 桥（按需单独构建） | `MeguvizMemoryComputeReadBridge::read_bridge`（或 `::arena_read_bridge`）<br>`MeguvizMemoryComputeHostBridge::host_bridge`<br>`MeguvizMemoryMediaReadBridge::read_bridge`<br>`MeguvizMemoryAhbReadBridge::read_bridge`<br>`MeguvizMemoryGpuAhbBridge::gpu_ahb` / `MeguvizMemoryAhbGlesUpload::upload` | 仅在明确对接 `MeguvizCompute`、`MeguvizMedia` 或 Android `AHardwareBuffer`（API 26+）时单独引入；`adapters/packed` 为独立纯 Java 11 `PackedDanmakuReader`，无 Memory C ABI 依赖 |

**兄弟库分工边界**：MeguvizMemory 不替代 `MeguvizCompute` 的 GPU/Vulkan 调度与执行期同步，不接管 `MeguvizMedia` 的解码帧时间线，也不接管系统 `Bitmap`、HWUI 或全局 `malloc`/`new`。家族 `io.github.jichuo1.meguviz:meguviz-bom:1.2.0` 管理 33 个 `1.2.0` 制品，不包含 `mgmem-android:1.2.2`。

## 第 3 阶段：执行最小迁移与生命周期对齐

### 1. C++17 最小骨架（同步临时区 + 异步读借用）

```cpp
#include <mgmem/memory.hpp>

mgmem::domain domain;
if (mgmem::domain::create_arena(1024 * 1024, 64 * 1024, domain) != MGMEM_OK) return;

// 同步临时缓冲区：指针严禁逃出回调
mgmem_status s = domain.with_allocation(4096, [](void *ptr, uint64_t len) { /* init & use */ });

// 跨线程交接：入队前取得 lease，owner 可提前释放，由任务持有 lease 到最后一次读取
mgmem::owned_allocation owner;
if (domain.allocate(4096, owner) == MGMEM_OK) {
    mgmem::borrow read_lease;
    if (owner.acquire_borrow(MGMEM_READ, read_lease) == MGMEM_OK) {
        submit_task([lease = std::move(read_lease)] { use_bytes(lease.data(), lease.size_bytes()); });
    }
}
```

### 2. Android Java / Kotlin 最小骨架（托管域 + DirectBuffer + 安全关闭）

```java
try (ManagedMemoryDomain.Allocation slot = domain.allocate(4096, 16)) {
    try (ManagedMemoryDomain.Lease w = slot.borrowWrite()) {
        w.withDirectBuffer(buf -> { buf.order(ByteOrder.nativeOrder()); /* 仅回调内有效 */ });
    }
    try (ManagedMemoryDomain.Lease dst = otherSlot.borrowWrite();
         ManagedMemoryDomain.Lease src = slot.borrowRead()) {
        src.copyTo(dst, 0, 0, 4096); // 单次上限 1 MiB
    }
}
// 退出时在后台工作线程执行：停新任务 -> beginClose() -> 观察到 CLOSED -> forgetClosed()
domain.beginClose();
if (domain.state() == ManagedMemoryDomain.CLOSED) domain.forgetClosed();
```

### 3. 生命周期、所有权与回退铁律

- **释放与等待线程**：最后一次 `owner`/`lease` 释放、`mgmem_domain_collect_v1`、`mgmem_domain_destroy_v1` 与 `awaitClosed(timeout > 0)` 可能同步执行 provider 回收，必须安排在允许阻塞的工作线程，严禁在主线程或完成回调线程执行。纯 C 域关闭严格按 `begin_close` → `collect` → `drain` → `destroy` 顺序。
- **未初始化与视图边界**：原生 C/C++ 分配内容**未初始化**；Android `ManagedMemoryDomain.allocate` 会同步清零；原生向 Java 注册借用（`mgmem_android_borrow_register_v1`）须先初始化全部范围并传 `MGMEM_ANDROID_BORROW_INITIALIZED`。`withDirectBuffer` 交付的 `ByteBuffer` 及切片在回调退出后失效；`copyTo` 与 `write` 单次上限为 `1 MiB`（`1024 * 1024` 字节）。
- **外部导入与隔离态**：外部导入的 `managing_runtime_origin` 必须等于 `mgmem_runtime_origin_v1()`；同一物理 backing 只交给一个运行库，且 `external_id` 全局唯一。一旦 checked 释放或托管关闭返回 `MGMEM_PROVIDER_ERROR` / `QUARANTINED`，**禁止重试释放**，保留域及回调上下文并停止新操作。
- **平滑回退**：回退时仅停止向新策略分配或新建域，让存量域按原 `owner`/`lease` 路径自然结束，禁止通过超时强行释放仍被借用的内存。

> **按需深入阅读**：完整 CMake Target、Maven 坐标、头文件与 API 签名见 [API 与模块速查](references/api-and-modules.md)；存量写法对照、生命周期规则与排障清单见 [迁移对照与排障](references/migration-patterns.md)。

## 第 4 阶段：验证与交付清单

1. **消费方行为与边界验证**：
   - 用原有真实输入对比迁移前后的输出字节；逐一核验失败路径：超出预算拒绝（`MGMEM_OUT_OF_BUDGET`）、任务取消或拒收时 lease 归还、`withDirectBuffer` 回调外拒访、关闭顺序先释放后收尾。
   - 若启用 Arena，采集真实运行快照（`mgmem_snapshot_v1`）与影子预测对比：
     `python tools/snapshot_feedback.py --shadow shadow-<site>.json --snapshot snapshot.json --mode arena`
2. **本库宿主自检参考命令**（Windows MSVC 环境先经 `tools/host-build.cmd` 或已配置 toolchain）：
   - 核心与测试：`cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core --config Release && ctest --test-dir build/core -C Release --output-on-failure`
   - 评估工具链：`python tools/test_run_adoption.py` 与 `python tools/run_adoption.py --inventory ALLOCATION_SITES.json --policy ALLOCATION_POLICY.json --out-dir build/adoption-check`
   - Android Java 生命周期回归：`python tools/verify_android_pressure.py --out-dir build/pressure-java-ci --receipt build/pressure-java-ci/receipt.json`
3. **交付摘要要求**：向用户汇报时明确列出已迁移的分配点及选型依据、实际接入的 SDK/AAR 版本与 Target、回退开关或步骤、实际运行过的测试命令与结果；若记录资源指标，将耗时、底层申请次数、逻辑 `backing_bytes` 与进程 `RSS/PSS` 分栏说明，不把工具预览或宿主 stub 测试当成真机性能结论。
