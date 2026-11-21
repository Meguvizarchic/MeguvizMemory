# MeguvizMemory API 与模块速查（1.2.2）

## 1. 核心库与发布产物边界

| 模块 / 产物 | 版本 | CMake 包 / Maven 坐标 | Target / 主类 | 工具链与平台要求 |
|---|---|---|---|---|
| C / C++ 核心库 | `1.2.2`（ABI `1.1`） | `find_package(MeguvizMemory CONFIG REQUIRED)` | `MeguvizMemory::mgmem` | CMake `3.25+`、C11 + C++17、`Threads::Threads` |
| 原生压力控制器（静态） | `1.2.2` | `find_package(MeguvizMemory CONFIG REQUIRED)` | `MeguvizMemory::pressure` | 仅在自行封装原生压力控制器时链接 |
| Android 独立 JNI SDK（可选） | `1.2.2` | `find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)` | `MeguvizMemory::android_jni` | Android NDK 30、`ANDROID_ABI` 一致、共享 `libmgmem.so` |
| Android AAR（含 Prefab） | `1.2.2` | `io.github.jichuo1.meguviz:mgmem-android:1.2.2` | Java 类 + `mgmem-android::mgmem`、`mgmem-android::mgmem_pressure_jni` | `minSdk 27`、`compileSdk 37.2`、JDK `17`、AGP `9.4.1`、Gradle `9.8.2`；含 `arm64-v8a` / `armeabi-v7a` / `x86_64` |

**家族 BOM 边界**：`io.github.jichuo1.meguviz:meguviz-bom:1.2.0` 管理 Android / UI / Drawable / Ref / Hook / DexMeguviz 共 33 个 `1.2.0` 制品，**不包含** `mgmem-android:1.2.2`。Android 端使用 `mgmem-android:1.2.2` 时需单独引用本地 Maven 仓库或组装好的 AAR（`bindings/android/`）。

## 2. 可选跨库适配器（仅按需独立构建）

以下桥接项目均位于 `adapters/` 下，不改变 `MeguvizMemory::mgmem` 与 `mgmem-android` 的默认依赖图：

| 适配器目录 | CMake 包名 (`find_package(... CONFIG REQUIRED)`) | CMake Target | 头文件 | 适用场景与前提 |
|---|---|---|---|---|
| `adapters/compute` | `MeguvizMemoryComputeReadBridge` | `::read_bridge`（System）、`::arena_read_bridge`（Arena，需 `MGMEM_COMPUTE_BRIDGE_BUILD_ARENA=ON`） | `<mgmem/compute_read.h>`、`<mgmem/compute_read_bridge.h>` | 将完整、ready 的 Compute CPU READ lease 零复制导入 Memory（依赖 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE`） |
| `adapters/compute_host` | `MeguvizMemoryComputeHostBridge` | `::host_bridge` | `<mgmem/compute_host.h>`、`<mgmem/compute_transfer.h>`、`<mgmem/compute_stage.h>` | 以 Memory 借用创建 Compute host 资源，或通过 `mgmem_upload` / `mgmem_download` / `mgmem_stage_*` 经 Compute `COPY` 传输 |
| `adapters/compute_ahb` | `MeguvizMemoryAhbReadBridge` | `::read_bridge` | `<mgmem/ahb_read.h>`、`<mgmem/ahb_map.h>`、`<mgmem/ahb_slot.h>` | Android API `26+`：将单层 RGBA8 AHB 的 Compute READ/WRITE lease 同步映射为 Memory 外部存储 |
| `adapters/compute_gpu_ahb` | `MeguvizMemoryGpuAhbBridge` | `::gpu_ahb` | `<mgmem/gpu_ahb.h>` | Android API `26+`：协调 Memory READ 借用与 GPU-only AHB WRITE lease 的 fence 交接 |
| `adapters/gpu_ahb_gles` | `MeguvizMemoryAhbGlesUpload` | `::upload`、可选 `::guarded`（`MGMEM_AHB_GLES_GUARDED=ON`） | `<mgmem/ahb_upload.h>`、`<mgmem/ahb_upload_guarded.h>` | Android API `26+` + GLES3 EGL：将 Memory RGBA8 图像直接 `glTexSubImage2D` 上传到 AHB |
| `adapters/media` | `MeguvizMemoryMediaReadBridge` | `::read_bridge`、可选 `::arena_read_bridge` | `<mgmem/media_read.h>`、`<mgmem/media_read_bridge.h>` | 将 `MgmFrame` 的完整 CPU READ lease 导入 Memory |
| `adapters/packed` | 无 CMake / 未发 Central | 纯 Java 11 类 `PackedDanmakuReader` | `io.github.jichuo1.meguviz.memory.packed.PackedDanmakuReader` | 独立优化 `int[] flat + byte[] reply` 只读解析，与 Memory C ABI 无关 |

## 3. 核心 C ABI 与状态码速查（`<mgmem/mgmem.h>`）

- **版本与能力探针**：
  - `mgmem_abi_version_v1()` 返回 `(MGMEM_ABI_MAJOR << 16) | MGMEM_ABI_MINOR`（当前 `1.1`）。
  - `mgmem_runtime_origin_v1()` 返回当前装载运行库的路由标识，外部导入结构体的 `managing_runtime_origin` 必须等于目标运行库返回值。
  - `mgmem_capabilities_v1()` 位掩码：`MGMEM_CAP_SYSTEM` (1)、`MGMEM_CAP_ARENA` (2)、`MGMEM_CAP_CUSTOM_PROVIDER` (4)、`MGMEM_CAP_RW_LEASE` (8)、`MGMEM_CAP_EXPLICIT_COLLECTION` (16)、`MGMEM_CAP_EXTERNAL_IMPORT` (32)、`MGMEM_CAP_SOFT_BUDGET` (64)、`MGMEM_CAP_CHECKED_EXTERNAL_RELEASE` (128)、`MGMEM_CAP_ARENA_EXTERNAL_IMPORT` (256)。
- **域、分配与借用**：
  - `mgmem_domain_create_v1(options, &domain)`：`mode` 取 `MGMEM_SYSTEM` (0) 或 `MGMEM_ARENA` (1)；C 接口须设置 `struct_size`、`abi_major`、`abi_minor`。
  - `mgmem_allocate_v1(domain, bytes, alignment, &owner)`：对齐须为 2 的幂；返回内容**未初始化**。
  - `mgmem_borrow_acquire_v1(domain, owner, offset, length, access, &borrow)`：`access` 为 `MGMEM_READ` (0) 或 `MGMEM_WRITE` (1)；写借用独占同一 `allocation`。
  - `mgmem_release_v1(domain, owner)` / `mgmem_borrow_release_v1(domain, borrow.lease)`：归还 `owner` 后已有 `lease` 仍有效。
- **外部导入与预算**：
  - `mgmem_import_external_v1` / `mgmem_import_external_checked_v1`（仅 System 域）。
  - `mgmem_import_external_arena_v1` / `mgmem_import_external_arena_checked_v1`（仅 Arena 域，先检查能力位与符号）。
  - `mgmem_domain_set_budget_v1(domain, bytes)`：支持设为 `0` 或低于当前已用容量，不影响已存在 `owner`/`lease`。
- **复用与关闭**：
  - `mgmem_arena_reset_v1`（仅 Arena 且无活跃资源）、`mgmem_domain_trim_v1`（无活跃资源）。
  - `mgmem_domain_begin_close_v1` → `mgmem_domain_collect_v1` → `mgmem_domain_drain_v1(domain, timeout_ms)` → `mgmem_domain_destroy_v1`。
  - 快照：`mgmem_domain_snapshot_v1(domain, &snap, sizeof(snap))`、`mgmem_domain_import_snapshot_v1(domain, &imp, sizeof(imp))`。
- **状态码 `mgmem_status`**：
  `0 MGMEM_OK`、`1 MGMEM_INVALID_ARGUMENT`、`2 MGMEM_OVERFLOW`、`3 MGMEM_OUT_OF_MEMORY`、`4 MGMEM_OUT_OF_BUDGET`、`5 MGMEM_STALE_HANDLE`、`6 MGMEM_BUSY`、`7 MGMEM_IS_CLOSED`、`8 MGMEM_UNSUPPORTED`、`9 MGMEM_PROVIDER_ERROR`、`10 MGMEM_TIMEOUT`。

## 4. C++17 封装速查（`<mgmem/memory.hpp>`）

仅头文件、move-only、`mgmem::domain` / `mgmem::owned_allocation` / `mgmem::borrow` 均持有内部 `shared_ptr<detail::domain_state>`。

- **创建域**：`mgmem::domain::create_system(budget, out, provider = nullptr)`、`mgmem::domain::create_arena(budget, chunk, out, provider = nullptr)`。
- **同步分配与借用**：
  - `domain.with_allocation(bytes, [alignment,] body)`：分配并开一个全范围 `WRITE` 借用，回调收 `(void *, uint64_t)`，可返回 `void` 或 `mgmem_status`。
  - `owner.with_read([offset, length,] body)` / `owner.with_write([offset, length,] body)`。
- **异步与外部导入**：
  - `owner.acquire_borrow([offset, length,] access, borrow_out)`：将 `borrow_out` 移入异步任务。
  - `domain.import_external(...)`、`domain.import_external_checked(...)`、`domain.import_external_arena(...)`、`domain.import_external_arena_checked(...)`。
- **观测与异常处理**：
  - `domain.observe()` 返回 `domain_observation`（`snapshot`、`imports`、`quarantined`）。
  - `domain.quarantined()` 查询 checked 外部释放失败触发的粘性隔离态。
  - `mgmem::or_throw(status)` 抛出 `mgmem::status_error`。

## 5. Android Java / Kotlin API 速查（`io.github.jichuo1.meguviz.memory.android`）

- **`ManagedMemoryDomain`**（实现 `AutoCloseable`）：
  - 工厂：`system(budgetBytes)`、`arena(budgetBytes, chunkBytes)`。
  - 核心操作：`allocate(bytes, alignment)`（**会同步清零分配范围**）、`setBudget(bytes)`、`attachPressure(moderateBytes, criticalBytes)`、`beginClose()` / `close()`、`state()`（`OPEN=0`, `CLOSING=1`, `CLOSED=2`, `QUARANTINED=3`）、`failureStatus()`、`forgetClosed()`、`snapshot()`。
  - 运行时上限（每份 JNI 库）：`MAX_MANAGED_DOMAINS=1024`、`MAX_MANAGED_ALLOCATIONS=65536`、`MAX_MANAGED_LEASES=65536`、`MAX_CLOSE_GUARDS=65536`、`MAX_CLEANUP_WORKERS=32`，对应异常状态码 `256..260`。
- **`ManagedMemoryDomain.Allocation` / `Lease`**：
  - `borrowRead()` / `borrowRead(offset, bytes)`、`borrowWrite()` / `borrowWrite(offset, bytes)`。
  - `read` / `write`、`copyTo(destination, srcOffset, dstOffset, count)`、`withDirectBuffer([offset, length,] action)`。
  - 传输上限：`NativeMemoryLease.MAX_WRITE_BYTES = 1 MiB`、`MAX_COPY_BYTES = 1 MiB`、`MAX_RUNTIME_STAGING_BYTES = 16 MiB`。
- **原生侧桥接 Java (`NativeMemoryLease`)**：
  - 原生 `<mgmem/android_borrow_jni.h>`：`mgmem_android_borrow_register_v1(..., MGMEM_ANDROID_BORROW_INITIALIZED, &token)` / `mgmem_android_borrow_cancel_v1(token)`；Java 侧 `NativeMemoryLease.fromNativeRegistration(token)` 认领。
- **压力联动**：`ManagedMemoryDomain.Pressure`、`ManagedPressureBackend`、`MemoryPressureAdapter`（映射 `ComponentCallbacks2`：`5..14`→`MODERATE`，`15..19`→`CRITICAL`，`20..39` 忽略含 `UI_HIDDEN`，`40..79`→`MODERATE`，`>=80`→`CRITICAL`）。

## 6. 评估工具 CLI 参数（仓库根目录运行）

- `python tools/draft_inventory.py --root <dir...> --output inventory.draft.json`
- `python tools/run_adoption.py --inventory <sites.json> [--policy <policy.json>] [--trace <site_id>=<trace.jsonl> ...] [--chunk 4096] [--preset ...] --out-dir <new_dir>`
- `python tools/shadow_estimate.py --trace <trace.jsonl> --sweep`
- `python tools/snapshot_feedback.py --shadow <shadow.json> --snapshot <snapshot.json> [--mode arena|system] [--output feedback.json]`
