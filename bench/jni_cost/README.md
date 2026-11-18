# JNI/Java 层三点隔离实测（宿主 JVM，无真机）

测的是托管 JNI 适配层三个从未隔离测量的点。构建方式复刻
`tests/managed_host/CMakeLists.txt` 的 plain 单 DLL
（`src/mgmem.cpp` + `pressure.cpp` + `borrow_jni.cpp` + `workspace_jni.cpp`，
无 `MGMEM_MANAGED_TESTING` 钩子，输出名 `mgmem_pressure_jni`），
Java 侧只用生产类 `ManagedMemoryDomain` / `NativeMemoryLease` 的公开 API。
不改任何生产代码。

## 环境

- Windows 11 专业版 10.0.26300，i7-13790F（16 核），单线程测
- Microsoft OpenJDK 17.0.20.1（HotSpot），MSVC 14.51 Release /O2
- 手写 harness：批量计时、3 轮预热丢弃、7 轮取中位数；整进程跑 3 遍互核
- 原始输出：`run1.txt`（首遍，点 3 因借用错误中断）、`run2.txt`、`run3.txt`

## 命令

```
cmake -S bench/jni_cost -B bench/jni_cost/build -G "Visual Studio 18 2026" -A x64
cmake --build bench/jni_cost/build --config Release
java -Xmx512m "-Djava.library.path=bench/jni_cost/build/Release" \
  -cp bench/jni_cost/build/classes io.github.jichuo1.meguviz.memory.android.JniCostBench
```

（本机 cmake 用 VS 18 自带：`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`）

## 点 1：托管 allocate 的尺寸斜率（system 域）

机制（`workspace_jni.cpp` 的 `nativeAllocate`）：`mgmem_allocate_v1` 后无条件
借 WRITE → `memset(0, bytes)` → release，再发布 owner。契约要求清零。

| 尺寸 | ns/op（run2） | ns/B | 尺寸正比占比（拟合） |
|---|---|---|---|
| 64B | 628 | 9.81 | ~2% |
| 4KiB | 1,791 | 0.437 | ~42% |
| 64KiB | 23,512 | 0.359 | ~51% |
| 1MiB | 221,539 | 0.211 | ~87% |
| 16MiB | 3,097,575 | 0.185 | ~100% |

- 五点最小二乘斜率 3 遍：0.184 / 0.204 / 0.204 ns/B ≈ 4.6–5.1 GiB/s。
- 64B 实测 ≈ 630（run1 663）ns，即固定成本下限：JNI 跨越 + 注册表 +
  核心 allocate + 两次核心 borrow/release。
- 参照（非产品路径）：`Arrays.fill` 同一热数组 ≈ 0.052 ns/B（~18 GiB/s）。
  allocate 斜率比纯热 memset 慢约 3.5×，因为它摸的是新内存——斜率是
  「核心新页提交/首触 + memset 清零」的合并上界。

结论：**≥64KiB 起尺寸正比项过半，1MiB 87%、16MiB ~100%，大 allocate
就是被逐字节成本主导**；小尺寸（≤4KiB）则固定成本与逐字节各半以下。
拦截说明：在「不改生产代码」约束下无法拆开 memset 与核心新页成本
（清零写死在 `nativeAllocate` 内）。要拆需 C 层对照
（核心 `mgmem_allocate_v1` 本身不清零，同一尺寸测核心 allocate+release，
差值即 JNI 清零段）。

是否值得改：不值得直接动。清零是托管契约（`ManagedMemoryDomain.allocate`
文档承诺发布前清零，借用侧 `MGMEM_ANDROID_BORROW_INITIALIZED` 也依赖之），
去掉等于改 API 语义并把未初始化内存暴露给 Java 读。若将来真要省，方向是
让核心对「OS 新页本身为零」给出证明、JNI 跳过冗余 memset；风险是证明错一处
即信息泄漏，需真机与核心层联合验证，本轮只测出上限收益约为斜率中 memset
那部分（≤0.18–0.20 ns/B，真实份额未拆）。

## 点 2：write vs read 差分（同一 WRITE 借用，1MiB 分配）

机制（`borrow_jni.cpp` 的 `nativeCopy`）：read = 一次 `SetByteArrayRegion`；
write = staging 预算 CAS 预留 + `new jbyte[count]` + `GetByteArrayRegion`
进 staging + `memcpy` 进目标（双拷贝 + 每次堆分配）。staging 是为失败原子性：
分配/准入失败时原生字节不变（`NativeMemoryLease.write` 文档承诺）。

| 尺寸 | read ns/op | write ns/op | 差 ns/op | 差 ns/B | 倍率（3 遍区间） |
|---|---|---|---|---|---|
| 4KiB | 100 | 157 | 55–63 | 0.013–0.015 | 1.5× |
| 64KiB | 1,079 | 2,315 | 1,194–1,336 | 0.018–0.020 | 2.1× |
| 256KiB | 4,084 | 8,514 | 4,401–4,853 | 0.017–0.019 | 2.1× |
| 1MiB | 18,425 | 295,545 | 247k–321k | 0.24–0.31 | 9.8–16× |

结论：

- 4KiB–256KiB：差值 ns/B（0.013–0.020）与 read 自身拷贝 ns/B
  （0.016–0.024）同量级，倍率稳定 ~2.1× → **差分就是多一次拷贝**，
  staging 小块堆分配的固定项被拷贝掩盖（≤几十 ns）。
- 1MiB 断崖：write 跳到 ~0.28 ns/B、倍率 10–16×，3 遍复现。归因不在拷贝
  而在 staging 的 1MiB `new[]`：每次走宿主堆大块路径
  （VirtualAlloc 量级 + 首触）。write 单次上限正好是 1MiB，断崖只出现在上限处。

是否值得改：

- ≤256KiB 的 2× 是失败原子性的明码标价，绝对值在 µs 级，不建议为它放弃
  「失败不改字节」承诺（直写 `GetByteArrayRegion` 进目标会在 JNI 异常时留下半写）。
- 1MiB 断崖是唯一像样的可改点：staging 复用/池化可望把 ~296µs 拉回
  一次拷贝量级（~40µs）。风险：池化与 16MiB 全局 staging 预算记账
  （`StagingReservation`）耦合，复用缓冲的记账与并发上限要重证；
  且断崖位置由宿主堆大块阈值决定，Android bionic malloc 阈值不同，
  **必须真机先复测断崖是否存在再决策**，宿主数字不可直接立项。

## 点 3：零字节固定成本 + 空借用生命周期

零字节时 `nativeCopy`/`nativeTransfer` 仍走完 lookup + entry 锁 + 全部校验
才在 `!count` 处返回，无数据触摸。「空借用」API 不允许 0 字节
（`bytes <= 0` 拒绝），故以最小合法借用 `borrowWrite(0,1)+close` 测生命周期，
其间含 5 次 JNI 跨越（borrow、claim、guardEnter、close、guardExit）。

| 项 | ns/op（run2 / run3） | ops/s |
|---|---|---|
| 空循环基线（未扣） | 0.3 / 0.3 | — |
| read count=0 | 71.0 / 71.9 | ~1.4×10⁷ |
| write count=0 | 73.0 / 70.1 | ~1.4×10⁷ |
| copyTo count=0 | 119.2 / 113.5 | ~8.5×10⁶ |
| read count=1 | 84.7 / 95.4 | ~1.1×10⁷ |
| write count=1 | 116.4 / 130.5 | ~8×10⁶ |
| copyTo count=1 | 113.8 / 128.0 | ~8×10⁶ |
| borrowWrite(0,1)+close | 779.9 / 756.0 | ~1.3×10⁶ |

结论：纯固定成本 read/write ≈ 71ns，copyTo ≈ 115ns（两次 lookup + 双把
entry 锁 + Java 层两个对象 monitor）。count=1 相对 count=0 的增量：
read +14~24ns、write +43~60ns（含 1B staging 分配 + CAS 预留）、
copyTo ≈ 0~14ns。空借用全生命周期 ≈ 770ns ≈ 130 万次/秒。

是否值得改：不值得。三项都是亚 µs 固定成本；借用生命周期的 770ns 里
5 次 JNI 跨越按零字节单价估仅 ~350ns，其余为核心 borrow/release 与
注册表增删。任何合并跨越的改法（如借用关闭并 guard）都动已验证的
隔离/看门狗协议，收益与风险完全不成比例。

## 口径与未测项（诚实清单）

- 宿主是 HotSpot + MSVC 堆 + Windows 页管理；Android 的 ART、bionic malloc、
  Linux 页语义都不同。**绝对值不可外推到真机**；可迁移的是结构结论：
  allocate 大尺寸逐字节主导、write 约 2× read、固定成本亚 µs。
- 点 1 只测 system 域；arena 域未测（清零路径相同，但核心分配器不同，
  斜率中「新页」份额会变）。
- 点 1 的 memset 份额未拆（见上「拦截说明」），拆分需 C 层对照基准，
  不在本目录范围。
- 无 JMH：native 调用不会被 JIT 消除，批量计时 + 中位数足够支撑量级结论；
  微秒以下的逐项数字请按 ±20% 读。
- 单线程口径：注册表锁、staging CAS 的并发争用成本未测。
