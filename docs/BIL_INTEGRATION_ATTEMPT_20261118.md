# BIL 全量接入尝试实战记录（2026-11-18）

结论先行：**依赖接得进、编得过、打包与 R8 都成立，但当前没有一个 BIL 生产热点能真正迁到 Memory 并获益**；阻塞点不在构建兼容，而在未发布、Java 侧无 ByteBuffer/地址互操作、AAR 无 prefab 三件事。本批按既定约束未用真机；不构成性能或 BIL 接入资格，[BIL_ADOPTION_DECISION.md](BIL_ADOPTION_DECISION.md) 的「不接入」结论在补齐下述前置前继续成立。

实验载体：BIL `main@fe54f9c5` 的隔离 worktree 分支 `codex/mgmem-full-integration-20261118`（提交 `91782d37`，未推远端），主树并行改动未动。基线为固定提交，不宣称主树当前状态已验收。

## 能力与兼容性核对（实测）

| 面 | Memory 1.2.1 现状 | 与 BIL 的关系 |
|---|---|---|
| 坐标与分发 | `io.github.jichuo1.meguviz:mgmem-android:1.2.1`，仅本地暂存，未上 Central，未进 BOM 1.2.0 | BIL 仓库集合无此源；必须先发布或本地暂存覆盖 |
| Java API | ManagedMemoryDomain（System/Arena、先清零、lease read/write/copyTo≤1 MiB）、压力绑定、原生借用 | Kotlin 可直调；但**不暴露地址/DirectByteBuffer** |
| Native API | C ABI 1.1 + C++ 头；AAR 只有 `classes.jar` 与 2 个 .so，无 prefab/头/CMake | BIL CMake 侧无法用 AAR 链接；须另用候选 SDK 或仓内头+手工 -L |
| 平台 | minSdk 27、compileSdk 37（minor 2）、Java 17、3 ABI、16K 对齐 | 与 BIL 37/27/17 一致；BIL abiFilters 仅双 ARM，x86_64 被过滤 |
| 打包/混淆 | AAR 自带 consumer rules（整类 keep） | AGP 9.5.1 消费 9.4.1 产物正常；R8 后 JNI 类与方法名保留（mapping 已核） |
| 加载 | ManagedMemoryDomain 静态块 `System.loadLibrary("mgmem_pressure_jni")` | 与 BIL danmaku 同为 STORED 直载路线；首次触类即载库，必须在安装期/后台完成 |

## 尝试与结果（全在开关 `-PinnocentLab.mgmem=true` 下，默认构建零变化）

接入形态照 BIL 既有 opt-in 先例（baselineprofile/Lumen sourceDir）：settings 仅在
`-Pmgmem.localRepo` 给定时加暂存仓库；依赖与 `src/mgmem` 源码仅开关打开时编入；
代码是三态闩锁 `MgmemAvailability`（纯逻辑、探针注入）+ 真实回环自检 `MgmemRuntime`
（建域→两分配→write→copyTo→read 校验→全关），与 `DanmakuWireNative` 同范式。

| 检查 | 结果 |
|---|---|
| `:app:testDebugUnitTest --tests *MgmemAvailabilityTest*`（开关开） | 4/4 通过（替身探针，未触 JNI） |
| `:app:assembleDebug`（开关开） | 通过；APK 内双 ARM 共 4 个 mgmem .so 均 STORED，合计约 567 KB 未压缩 |
| `:app:minifyReleaseWithR8`（开关开） | 通过（4m35s）；mapping 证 JNI 类名与 native 名保留 |
| `:app:compileDebugKotlin`（开关关，默认） | 通过；默认构建不可见 mgmem 源码与依赖 |
| `:app:contractsFor changed` | 通过但报「No changed production Kotlin files」——opt-in 源目录不在其扫描面，属**盲区而非覆盖** |
| NDK clang 手工链接探针（仓内头 + AAR arm64 .so，`-fPIC`） | 链接通过，DT_NEEDED 含 libmgmem.so；探针侧另拉 libc++_shared（消费者自选 STL；libmgmem 自身只依赖 libm/libdl/libc） |
| 采用工具盘点（draft_inventory 扫插帧/弹幕/cpp） | 0 站点——扫描器只认已用 Memory 的调用，未迁代码base 的候选仍靠人工清单（工具改进点） |

## 逐站点结论（对当前 BIL 代码复核）

| 站点 | 分类 | 实战结论 |
|---|---|---|
| FrameScratchArena（五槽 direct buffer） | excluded（已有复用） | 即便不排除也迁不了：缓冲以 DirectByteBuffer 交给 JNI/ncnn，Memory Java API 不给地址/ByteBuffer，改走 lease 必然多一次拷贝 |
| FrameBufferLedger | excluded | 同上，且帧 backing 寿命归账本所有，不可提前回收 |
| frame_guard_geometry / guard | excluded | 栈上计算，无堆分配点 |
| frame_engine staging / ncnn | excluded | GPU/ncnn owner 原地保留；且 C++ 侧无 prefab 可链 |
| frame_engine 局部容器 | system 方向 | 须活过 submit，无 Java owner 注入点；C++ 侧需 SDK 手工链接，未产品化 |
| Danmaku flat→Java | uncertain | 输入已是 JVM byte[]，先拷进 lease 是纯成本；packed reader 才是正解（已在既有规划内） |
| DexKit 会话 | system 方向 | DexKit bridge 无外部 owner 参数，无注入点；独立 Dex 全枚举不是此生产路径 |

## 问题清单（按优先级）

1. **P0 未发布**：不上 Central（或进 BOM），任何干净构建都解不出依赖；实验靠本机暂存仓库 + 开关绕行，不可进主线。
2. **P1 ByteBuffer 互操作缺失（最大功能阻塞）**：帧管线的 Java↔native 合同就是 DirectByteBuffer。需要一个受 lease 寿命约束的 ByteBuffer 视图设计（或把所有权移到 native 侧经 C++ 域持有），否则 Java 侧迁移全是加拷贝。
3. **P1 AAR 无 prefab/头文件**：C++ 消费者只能手工拼 SDK。应随 AAR 发 prefab（或独立 native 工件），并把候选 SDK 的 CMake 包作为正式分发面。
4. **P2 JVM 可测性**：ManagedMemoryDomain 类初始化即 loadLibrary，纯 JVM 单测零覆盖真实路径；实验被迫拆闩锁/探针双类。建议提供官方 fake/testing 工件或可注入的加载点。
5. **P2 BIL 契约盲区**：contractsFor 不扫 opt-in 源目录；将来转正时须把 mgmem 源集纳入契约与覆盖分母，或回收开关改为常开。
6. **P2 Xposed 宿主实载未验**：STORED 与既有 danmaku 路线一致只是推断；宿主进程实载、16K 真机 loader、armv7 仍缺（本批禁真机，未做）。
7. **P3 体积与 keep 粒度**：整类 keep 保守但可用；三 ABI 中 x86_64（约 329 KB）对双 ARM 应用靠 abiFilters 剔除，无 splits 时靠 Play/分发侧处理。

## 建议的解锁顺序

发布 Central 并进 BOM → AAR 补 prefab + 头 → 决策 ByteBuffer 互操作（lease 约束视图或 native 持所有权）→ 出 JVM fake 工件 → 用同一实验分支重跑并补设备轮。在此之前，BIL 保持现有实现；本分支作为可复现的接入脚手架与问题证据保留，不合并主线。

## 同日改进追记

上列 P1 两项已在同日改进中落地并复验：AAR 现带手写 Prefab 包（schema 1、双模块双 ABI 头与库；AGP 自带 prefabPublishing 对纯预置库模块不可用，改为生成 prefab 树合并进 AAR），BIL 实验分支以 `find_package(mgmem-android)` 探针端到端构建通过（探针 .so 入 APK、DT_NEEDED 仅 libmgmem 与系统库）；lease 新增作用域受限 `withDirectBuffer` 零拷贝视图（监视器保活、自关拒绝、返回失效），宿主 JVM 50/50 通过。重建候选时另修两处：候选构建器的方法名正则不认点分返回类型；以及新发现 **BIL 的 gradlew.bat 恒设 `JDK_JAVA_OPTIONS`，其 NOTE 行会被 AGP 的 prefab 校验当致命错误**——本机任何 Prefab 消费在修 wrapper 前都会在 configureCMake 挂掉，本批靠绕过 wrapper 直跑 Gradle 发行版验证，BIL 侧需把调优参数迁出 `JDK_JAVA_OPTIONS`。P0（Central 发布与 BOM）与设备验证仍未做，解锁顺序其余项不变。
