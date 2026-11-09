# MeguvizMemory 工程状态

最近补充：2026-11-09。当前交付 **0.2.0 功能工程预览 / native ABI 1.1**。公开仓库为 [jichuo1/MeguvizMemory](https://github.com/Meguvizarchic/MeguvizMemory)。正式 Release、性能资格和 BIL 接入尚未完成。

## 紧凑句柄表与稳定登记引用（2026-11-09）

已采用owner/lease共用的单内联记录+平坦表，以及紧凑全局claim节点引用；基线a1bf94c/C8，
选定核心890e1109…，不改变公开ABI/ID/权限/预算与不确定回调隔离合同。
组合832进程独立重算：Arena main六组workflow P50比0.7913–0.8729且各8/8改善，tiny fixed/mixed
0.8452/0.8886且各8/8改善，System十二组P50中位比均<1，reverse/shuffled主要组0.4236–0.6034。
一次性部分held完整时段、single mixed分配尾部和System部分P95仍有代价，全部保留；首版lease-only
另832进程独立保存，不混池。已实测domain456→544B、Backing仍80B，两表增长combined峰值
held32/64为5376/10752B，空批次归还数组，不称RSS下降。
选定源码核心/JNI48/48、C++12/12、ASan48/48、直接表8项/4238156检查/4次OOM及pending重定位通过；
六个arm64 ELF仅交叉构建/静态通过，没有设备、新Android制品、BIL或正式性能资格。
[当前实现与取舍](docs/HANDLE_TABLE_OPTIMIZATION.md) · [完整证据](artifacts/handle-table-20261109/decision.json)。

## 历史 Arena 小负载固定成本与结构实验（a1bf94c，2026-11-09）

先完成128进程未修改基线成本诊断及真实allocator计数，再实现一个Arena闲置lease节点复用。
重复stream的lease节点申请128→1；domain456→472B，另最多保留一个56B闲节点，trim/collect释放。
候选保存于本地`codex/arena-lease-reuse-experiment-20261109`/`7abf79f`，不切换默认核心。
832进程/416对/58组独立原始重算：重复stream完整round-wall P50比0.8785–0.9395、各8/8改善；
System32域部分组全部4对变慢约5%–6%，单次held/worker及cold也有明确代价，所有反向保留。
当时主线运行库保持40c7b1f/c8be3d52…；只纳入诊断/配对工具、公开行为回归与证据。
候选核心46/46、C++12/12、ASan46/46及arm64静态通过；基线加载新公开lease回归2项/2476检查通过，
主线重新构建46/46及成本工具A/A通过。本轮没有设备运行、新Android制品、BIL或产品性能资格。
[诊断与取舍](docs/ARENA_SMALL_WORKLOAD.md) · [全部证据](artifacts/arena-small-20261109/decision.json)。

## Arena mixed 优化与小负载限制（2026-11-09）

已采用最小请求支配下界复用：尺寸与规范化对齐均不小于缓存请求时复用 first-fit 位置，
不增加状态或修改公开 ABI。基线 `988ea2c`，选定核心 `c8be3d52…`。
首批原八对 main mixed 1/4线程 allocation P50 比0.6604/0.4951，workflow比0.8966/0.7610，
P50/P95均8/8改善；tiny fixed仍有workflow 1.0268倍、+10.55ns/op配对代价，小负载收益未证实。
通知门控及两类map提示撤出，三批576/416/256进程独立审计并完整保留，不剔样或混池。
新增独立prepared-single，计时前预备1个4096B@64块，前后provider=1；原tiny是2～4块。
选定源码重新通过核心/JNI45/45、C++12/12、ASan45/45与arm64交叉构建/ELF静态门禁；
本轮没有设备运行、Android新制品、BIL或正式性能资格，也没有所选版本的System性能实测。
首轮ASan缺子进程运行库PATH导致的超时与修复后通过记录分别保留。
[完整采用说明](docs/ARENA_MIXED_OPTIMIZATION.md) · [全部证据](artifacts/arena-mixed-20261109/decision.json)。
长期目标在本轮范围之外继续保持暂停。

## 历史稳定登记引用实验收尾（988ea2c，2026-11-09）

本轮已完成独立审计与采用决定：两项缓存调校否决；稳定登记引用 v2 保留于
`codex/claim-reference-experiment-20261109`（核心提交 `f9882d8`），不进入通用默认。
当时主线核心保持 `d2b6aa3` 的字节，新增基准、first-fit 回归与 CI 功能回执。
host v1、host v2、arm64 v2 各 160 进程／80 对／20 组独立重算，全部轮次保留。
逆序／打散释放有改善，但 arm64 Arena mixed 四线程 workflow 比 1.378（4/4 对变慢），
tiny fixed 比 1.420；并发 mixed 存在峰值 backing 差异，不可将代价完全归因于索引指令。
现有核心 23/23、C++ 12/12、ASan 23/23 与 arm64 三模式功能证据均核对匹配，
不因此取得正式产品性能资格。本次只离线收尾，未新增设备命令、制品或 BIL 变更，
长期目标仍暂停。[采用决定与完整证据](artifacts/claim-release-20261109/decision.json) ·
[实验说明](docs/CLAIM_REFERENCE_EXPERIMENT.md)。

主线收尾后重新构建验证：核心及可选 JNI 45/45、C++ 12/12，两个安装后基准的 A/A 功能配对各 12 个进程通过。[本次宿主回执](artifacts/claim-release-20261109/main-host-validation.json)。

## Android 新制品与真机验证（2026-11-09）

本轮源码提交 `6968ed1` 的 [Windows/Linux CI](artifacts/android-round-20261109/ci-6968ed1.json) 均通过；设备结论来自下述独立真机回执。

已从固定 `bfd0e65` 生成包含 copyTo 与 Arena 查找优化的三 ABI 工程候选及通用 AAR。三 ABI 静态门禁通过；API33/arm64/4KiB 原生13命令（12正常、1预期隔离）与 R8 5case/1178检查通过。R8实测2个copyTo case、96次Arena同形申请、关闭后归零；0 cgroup样本仅表示未观察。通用AAR只取得同批成员继承，尚未由它单独构建运行APK。[候选与运行证据](artifacts/android-round-20261109/)、[快速接入](docs/ANDROID_QUICKSTART.md)。

原生Arena配对48进程/24对完整保留。main同形1/4线程allocation批次P50配对比0.497/0.667；混合4线程存在一对5.540倍allocation与1.526倍workflow退化。tiny混合allocation四对均变慢，P50中位比1.057、P95比1.217。各scope不混池，不剔样，不作为产品或正式性能资格。本轮是Memory库内部游标前后对照，不计为引入Memory的产品净收益，也不重复计入BIL既有scratch优化。首轮push进度stderr误判停在0样本暂存阶段，原记录保留。下一优化重点为mixed/tiny路径开销及多线程尾部，而非仅扩大有利负载。[全部配对结果](artifacts/android-round-20261109/arena-pairs.json)。

多用户安装准入已区分全局已知包与实际安装，CRLF输出可解析，支持复用自有诊断签名；三轮安装前受阻记录保留，随后同签名更新数据保留。新制品未取得其他ABI运行、真实16KiB loader、MTE、长稳、真实系统压力或BIL资格。长期目标保持暂停。

## Arena 查找快路径（2026-11-09）

代码提交 `c74bc39` 的 [Windows/Linux CI](artifacts/arena-lookup-20261109/ci-c74bc39.json) 均通过；该结论对应宿主构建与合同，不代表设备运行。

保持 first-fit 的同形游标已实现，不改变公开 C ABI 或调用方式。核心 22/22、独立模式 177 检查及新运行库安装 DEX 7 用例通过。默认32进程配对中，同形1/4线程 allocation P50 配对比为0.534/0.325，workflow比0.846/0.680；混合请求未见稳定收益。短轮 tiny 混合 allocation 比1.64，独立预声明32进程确认轮未稳定复现该退化，但尾部与时序变化仍在，不能称无回归；全部轮次保留于[证据](artifacts/arena-lookup-20261109/host.json)。这只是本机合成探索，非产品或 Android 资格。[复跑合同](bench/ARENA_LOOKUP.md)。

## Lease 直接传输（2026-11-09）

Java 及托管 Lease 新增 `copyTo`，普通 JNI 直接 memmove，避免 Java 中转数组和 write staging。借用宿主 2/2、托管域 50/50 专项通过，包含双向并发、自重叠和 CLOSING 期间持 lease 传输。64 KiB、复用数组基线的单进程宿主 12 对探索性测量，candidate/baseline 配对中位比 0.32531；CPU/频率未固定、无独立 JVM 重复，不构成 Android 或产品收益。[证据](artifacts/lease-transfer-20261109/host.json)、[复现](bench/LEASE_TRANSFER.md)。新 Java/JNI 须配套部署，旧 JNI 不支持新 nativeTransfer。

## Arena 外部导入功能收尾（2026-11-09）

显式 Arena 导入、checked 释放和 C++ 入口已实现；外部 backing 与自有 chunk 共用预算且不会进入 Arena 子分配。独立 DEX 消费者的工作区实际参与完整结果 checksum。迁移后新构建、安装验证及旧消费者回归通过：[宿主证据](artifacts/arena-import-20261109/host.json)，核心 21/21、C++ 12/12、Arena 消费者 6 项加 direct 对照；无新设备或性能资格。

## 压力关闭准入竞争修复（2026-11-01）

- 修复 Java backend 和原生 JNI 两层竞争：后台线程启动尚未确认时，并发关闭不得误报已受理；明确失败可重试，JNI 异常未知时不盲目重试，独立完成回调可确认受理。极端线程已启动但 detach 抛异常时保守保留线程对象和可能的系统句柄。[合同](docs/ANDROID_PRESSURE.md)
- [宿主旧新对照](artifacts/pressure-close-admission-20261101/host.json)复现两层旧版误报；修复后 Java fake-JNI 26 项断言、native JNI 22 项用例、完整核心 42 项及 framework stub 38 项通过，真实宿主 JVM/JNI 组合验证 24 周期、264 项检查，真实 Android SDK 源码编译通过。此前 `2e25092` Android 候选、`085cc13` Linux 候选和真机短测均早于本修复，不能作为修复后制品或设备资格。
- 修复提交 `ce2a5b2` 的 [Windows/Linux CI](artifacts/pressure-close-admission-20261101/ci-ce2a5b2.json) 均通过，Windows 本机 JNI 相关 ASan 22 项通过；这些仍不是 Android 设备验证。
- 修复提交已生成[新 Android 三 ABI 和 Windows 宿主工程候选](artifacts/pressure-close-admission-20261101/candidates-ce2a5b2-corrected.json)，源码锁各含 306 项；Android 的 SDK/AAR 对应原生库一致并通过同 ABI 编译链接，Windows 安装消费者通过限定功能检查。原摘要混用了源码锁记录数组与包内源码字典的哈希口径，保留[原记录](artifacts/pressure-close-admission-20261101/candidates-ce2a5b2.json)并以更正记录供诊断工具核验；逐项源码及候选包未变。
- 随后 `182cc7c` [Linux 宿主候选](artifacts/pressure-close-admission-20261101/linux-host-182cc7c.json)已通过 CI 安装消费者及下载回读；修复版 arm64 托管短测和长稳诊断 APK 已[R8 构建并完成无设备冻结检查](artifacts/pressure-close-admission-20261101/managed-r8-build-ce2a5b2.json)，但这两个托管诊断不直接覆盖本次修复的 NativePressureBackend；专用真机短测见下，其他设备和性能仍待验证。
- 新增独立 [NativePressureBackend 关闭真机诊断](docs/ANDROID_NATIVE_PRESSURE_DEVICE_PROTOCOL.md)，覆盖合成压力、双线程关闭和持有借用后的归零，但不保证命中特定竞态。宿主 2 项、构建器 10 项、运行器 20 项离线检查通过；arm64/x86_64 诊断 APK 完成 R8 构建及虚拟目标冻结。[准备证据](artifacts/pressure-close-admission-20261101/native-probe-preparation.json)不代表设备执行或长稳资格。
- 诊断提交 `f109a8a` 的 [Windows/Linux CI](artifacts/pressure-close-admission-20261101/native-probe-ci-f109a8a.json) 均通过构建器、运行器及宿主桩检查；未进行设备操作。
- 随后确认其他会话释放设备后，修复版 NativePressure 独立诊断的[首次安装](artifacts/pressure-close-admission-20261101/native-probe-first-install.json)返回系统安装受限及取消，instrumentation 未启动；后续只读观察两名可见用户中无该包。具体取消原因未知，该轮没有取得设备结论。
- 用户确认后重新协调窗口并重试，修复版 [NativePressure API33 arm64/4KB R8 真机短诊断](artifacts/pressure-close-admission-20261101/native-probe-arm64-short-pass.json) 4 周期、53 检查通过，持借用关闭后最终账目归零，安装与后验通过。0 个 cgroup 样本只表示未观察，不能证明无冻结；并发不保证命中特定窗口，长稳、真实系统压力和性能仍未资格。
- 修复版托管域[长稳首次安装](artifacts/pressure-close-admission-20261101/managed-stability-first-install.json)被系统取消，前台 Activity 和 instrumentation 均未启动；只读观察两名可见用户均无该包。该轮没有长稳结果，具体取消原因未知。
- 用户确认后重试托管域长稳，本地 runner 将该轮自身启动器误判为其他测试并中断传输；设备端进度自报 900.098 秒/3007 周期，但终态同次采样观察到进程冻结，先后顺序未知。Activity 与 instrumentation 已结束，冻结进程仍存在；[只读观察](artifacts/pressure-close-admission-20261101/managed-stability-interrupted-observation.json)不能补足原协议终态，整轮 INCOMPLETE。用户已说明手机安装有冻结模块；该次采样未独立识别触发者。
- 修复版 [arm64 安全、DEX 和图像目标](artifacts/pressure-close-admission-20261101/arm64-consumer-cross-build.json)已完成 10 个目标交叉构建及安装核心链接回读；首次后验检查因辅助正则误匹配失败，修正后重跑通过，原始失败 stdout 未保存。交叉构建不构成设备或性能验证。
- 长稳 runner 的自身启动器误判和 PID/UID 跨快照身份检查已修复，[38 项离线合同](artifacts/pressure-close-admission-20261101/stability-launcher-fix-host.json)通过；真实设备 `/proc` 可读性及修复后新轮仍未验证，原中断轮不因修复升级。
- 新增[私有 ARM64 输入 profile](docs/ARM64_INPUT_PROFILE.md)，绑定选定修复版制品、源码和工具入口；[10 项宿主合同及本地自一致检查](artifacts/pressure-close-admission-20261101/arm64-profile-host.json)通过。它尚未接入旧设备 runner，也不冻结完整环境或证明加载/性能。
- 新增独立 profile 功能验证入口，旧 runner 字节保持不变；[18 项离线合同](artifacts/pressure-close-admission-20261101/arm64-profile-consumer-host.json)、旧入口 13 项及本地制品映射通过。新收据采用独立 scope 并被旧性能冻结器拒绝，尚未在设备运行或接入新性能冻结协议。
- 该入口随后补足本地传输启动、退出观察和管道收尾跨截止的失败处理，[20 项离线合同](artifacts/pressure-close-admission-20261101/arm64-profile-consumer-deadline-host.json)通过；单次系统调用不保证硬截止，设备端仍按未知处理。
- 上述 runner 和 profile 提交 `b9ada4c` 的 [Windows/Linux CI](artifacts/pressure-close-admission-20261101/stability-profile-ci-b9ada4c.json) 分别通过 38 项和 10 项离线合同；修复后 runner 的设备运行仍待重新协调验证。
- 独立 profile 功能入口现持久绑定制品、输入、运行工具和源码；新增[配对性能冻结与采集入口](docs/PROFILE_SUITE_FREEZE_V2.md)，在设备操作前校验功能明细、安全结果、授权、部署及同批身份。[本机离线证据](artifacts/pressure-close-admission-20261101/profile-suite-preparation-host.json)：功能入口 27 项通过，冻结器 15 项通过、1 项因系统不允许符号链接而跳过，采集入口 15 项通过；旧入口回归通过。明细核验不是签名认证，源码闭包不是连续监测，设备与性能均尚未运行或取得资格。
- 上述入口提交 `f19ebe4` 的 [Windows/Linux CI](artifacts/pressure-close-admission-20261101/profile-suite-ci-f19ebe4.json) 均通过；它们属于宿主构建与离线验证，不改变真机长稳和性能状态。

## 统一验证准备（2026-11-01）

- 新增[托管域真机诊断与冻结协议](docs/ANDROID_MANAGED_DEVICE_PROTOCOL.md)：System/Arena 的预算、合成压力、跨关闭 lease、最终账目与回调关闭采用独立 instrumentation；同一源码的宿主 JVM/JNI 加 Android 桩 2 项通过。arm64 与 x86_64 诊断 APK 已完成 R8 构建及 37 个 JNI 声明检查；arm64 后续真机短测见下，x86_64 尚无本诊断运行证据。[构建限定证据](artifacts/managed-probe-20261101/build-only.json)
- 独立 runner 已完成假 ADB 的准入、签名、冻结、超时和后验离线回归；真实冻结计划默认仅写入被忽略的 `build/`。首轮真机安装返回 `INSTALL_FAILED_USER_RESTRICTED`，诊断没有运行；后续只读观察在可见用户中未发现该包，具体取消原因未知。[脱敏记录](artifacts/managed-probe-20261101/device-install-incomplete.json) 设备观察不构成共享锁，短诊断也不能作为 900 秒资格。
- 随后重试安装并完成 [API33 arm64/4KB 的 R8 真机短诊断](artifacts/managed-probe-20261101/device-short-pass-20261101.json)：3 组、306 项检查通过，覆盖合成压力、预算和关闭，最终 backing 为 0；安装包身份和运行后核验通过。transport 耗时 0.79 秒；进程 cgroup 获得 0 个样本，`NOT_OBSERVED` 不表示未冻结。该结果不证明长稳、真实系统低内存、性能、BIL 或其他 ABI。
- 新增[Linux 宿主候选隔离 CI 入口](docs/LINUX_HOST_CANDIDATE_CI.md)，按指定提交从源构建并验证安装消费者。[首轮](artifacts/managed-probe-20261101/linux-host-first-attempt.json)、[第二轮](artifacts/managed-probe-20261101/linux-host-second-attempt.json)均失败且未上传制品；[第三轮](artifacts/managed-probe-20261101/linux-host-third-attempt.json)的受控诊断将静态库处理错误归类为未知选项，但不能独立证明根因。构建器此前解析工具符号链接可能改变 LLVM 多调用程序的模式，现保留请求的入口名称并新增回归；本机 22 项通过、1 项 POSIX 专属测试跳过，修复后的 Linux 结果见下。runner 的 34 项离线检查通过；CI sentinel 不等同用户本机私有标识扫描。
- 修复后的 `085cc13` [Linux x86_64 工程候选](artifacts/managed-probe-20261101/linux-host-candidate-pass.json)已在 GitHub-hosted Linux 构建并通过安装消费者检查：图像默认 12 组、合成压力 12 组、负例 10 组及 pressure v1/v2；43 步的 86 个日志流摘要经校验。Actions 制品下载回读的大小和哈希匹配，另存本地独立候选。CI 标识扫描不等于用户本机私有标识扫描；设备、性能和正式发布不因此取得资格。
- 独立[托管域长稳诊断与采集协议](docs/ANDROID_MANAGED_STABILITY_PROTOCOL.md)已补齐：包含已知前台身份、预算和关闭循环、停滞优先、保守采样时间和自身窗口退场检查。宿主 8 项、构建器 11 项及采集器 30 项离线检查通过；arm64 与 x86_64 的 R8 APK 已本机构建并以虚拟设备标识完成无 ADB 冻结检查。[限定记录](artifacts/managed-probe-20261101/stability-preparation.json)不代表 Android 运行、900 秒设备长稳、真实系统压力或性能资格。
- 长稳工具提交 `8939ab6` 的 [Windows/Linux CI](artifacts/managed-probe-20261101/stability-ci-8939ab6.json) 均通过，包含构建器、采集器与宿主测试；它们不是设备长稳结果。

## 修复后工程候选（2026-11-01）

- 从提交 `2e25092` 的独立源码快照分别生成本地三 ABI Android 候选和 Windows x86_64 宿主候选；两包的 261 项源码记录与该提交一致。包名、SHA-256 和[限定证据](artifacts/delivery-20261101/final-candidates-2e25092.json)已固定，候选仅留本地 `dist/`，不等于正式 Release。
- Android 的三个 AAR/SDK 内核心与 JNI 字节分别一致；真实图像、压力图像及显式 `android_jni` 目标均完成三 ABI 交叉编译和链接，arm64 AAR 完成 Android SDK 示例源码编译。该交叉编译环节没有运行 Android 可执行文件、ART 或设备。[安装消费记录](artifacts/delivery-20261101/android-installed-link-2e25092.json)
- Windows 包从归档回读后运行真实 Java 图像默认 12 组、合成压力 12 组、失败 10 组，以及 pressure v1/v2 消费者；37 个步骤的 74 份私有日志均与 journal 摘要一致。此前 `4aa2874` Windows 旧轮的覆盖记录不被改写。该阶段尚无 Linux 宿主候选，后续结果见上节。
- [最新源码 Windows/Linux CI](artifacts/delivery-20261101/ci-host-2e25092.json) 均通过；两端候选构建器的 29 项合同检查均全部执行，包括三个真实日志符号链接负例。本机 Windows 因权限跳过三项的原记录保留。上述限定 arm64 R8 短测以外的设备和 R8 运行矩阵，以及 16 KB loader、MTE、性能、BIL 和正式发布资格仍独立待验证；项目许可证尚未确定。

## 压力并发安全与 JNI 安装目标（2026-11-01）

- 修复 Android 回调适配器并发 `close()`：同线程重入继续由外层处理；另一线程遇正在执行的关闭会得到可重试异常，不会把未完成的注销或清理准入误报为成功。`isClosed()` 只表示停止事件准入。[宿主新旧反例](artifacts/pressure-races-20261101/host.json)
- 修复 pressure v1/v2 控制器并发创建时先取快照、后占附着槽导致的过期预算：现在先预占唯一附着，再读取预算；失败时在锁外归还 owner。四种版本交叉竞争的旧失败与新通过、失败回滚及回调锁序均有限定宿主证据。
- Android SDK 新增可选的 `MeguvizMemory::android_jni` 安装目标，core-only 包仍可使用；显式组件会校验架构、ELF 和同安装前缀。配置合同 32 项通过；构建器补强日志保护后，本机 26 项离线合同通过、3 项因权限跳过，随后两端 CI 的 29 项全部通过。最终源码制品见上节，运行验证仍待完成。
- 此前本地 `c45723f` 工程候选早于上述修复与 JNI 目标，不能作为修复后的新版制品。更广设备和 R8 运行范围、真实低内存和性能资格仍待统一验证。

## 图像访问轨迹工具（2026-11-01）

- 新增[独立 DEX 元数据 reader 轨迹诊断](consumers/dex_trace/README.md)，仅在隔离源码副本插桩固定读取点；不修改共享 DexMeguviz。宿主用诊断 APK 的 DEX 对照原样与插桩 reader，摘要一致并记录 19401 次读取、命中 23/32 个静态位点；损坏输入与事件上限负例、13 项离线合同检查通过。[限定证据](artifacts/dex-metadata-trace-20261101/host.json)不代表 Memory 消费者、DEX 查询全路径、产品轨迹或性能。`182cc7c` 的 [Windows/Linux CI](artifacts/dex-metadata-trace-20261101/ci-182cc7c.json) 均通过 12 项并各跳过 1 项缺少外部源码的检查。
- [真实 DexMeguviz 配对消费者的隔离 reader 采集](consumers/dex_trace/PAIRED_CONTRACT.md)已接入安装版 Memory：使用诊断 APK 的 DEX，四种输入策略与五类场景共 20 对原样/插桩宿主对照通过；正常及跨线程场景每次记录 19401 条、命中 23/32 个位点，坏输入和 lease/预算拒绝的事件数符合预期。[证据](artifacts/dex-metadata-trace-20261101/paired-host.json)仅覆盖 snapshot-open 调用线程的元数据 reader，不涵盖完整查询、产品输入或性能；模块路径由子进程自报并与磁盘哈希对照。
- 配对采集 `fd420a2` 的 [Windows/Linux CI](artifacts/dex-metadata-trace-20261101/paired-ci-fd420a2.json) 均完整通过。新增[受控 reader 字节回放](consumers/dex_trace/REPLAY_CONTRACT.md)，先实际执行并绑定采集，再以独立 oracle 对照 20 对回放；10 个 System/Arena 合成域关闭归零，另 10 个普通路径只确认引用清除。[宿主证据](artifacts/dex-metadata-trace-20261101/paired-replay-host.json)不证明原始跨线程寿命、完整查询、产品性能或操作系统内存回收。
- 回放提交 `812ce41` 的 [Windows/Linux CI](artifacts/dex-metadata-trace-20261101/replay-ci-812ce41.json) 均通过外层 17 项和 worker 11 项合同；这些仍是宿主测试。
- 此前授权的真实 DEX 在首次[宿主有界尝试](artifacts/dex-metadata-trace-20261101/real-dex-first-attempt.json)中，首个基线完整消费 54147 个方法，但插桩侧返回 `trace_incomplete`，0/20 对完成、没有合格采集或回放回执。该轮的错误合并了多种原因，不能断言其已触发事件上限；不计性能或 BIL 收益。
- 采集器现可报告固定失败类别及有界计数，旧结果保留不变；[第二次真实 DEX 尝试](artifacts/dex-metadata-trace-20261101/real-dex-second-attempt.json)中子进程自报 `EVENT_LIMIT`：记录 100 万条、尝试下界 100 万零 1 条，0/20 对完成。该下界不是实际总数，也不是独立执行映像认证；需扩展有界采集能力，仍无性能资格。[分类合同检查](artifacts/dex-metadata-trace-20261101/failure-classification-host.json)
- 对同一完整真实 DEX 的 D-C0/normal 使用[常量事件空间计数诊断](artifacts/dex-metadata-trace-20261101/real-dex-count-only.json)，子进程报告 6113916 次选定 reader 读取、命中 28/32 个位点并完整消费 54147 个方法；仅用于该单场景容量参考，不是可回放轨迹或性能。该轮后另补计数工具的截止时间后验检查，不改历史回执。
- [独立 count-only 工具](consumers/dex_trace/count_profile/README.md)已以诊断 DEX 完成 20 对宿主对照并通过固定状态、溢出和截止检查；[限定证据](artifacts/dex-metadata-trace-20261101/count-profile-host.json)不将计数提升为可回放轨迹或性能结果。
- [独立 V2 流式采集与回放](consumers/dex_trace/stream_v2/README.md)使用固定缓冲、明确事件及磁盘预算、独立文件回读与合成域寿命；诊断 DEX 的 20 对已通过，磁盘及单轨迹限额负例均为 INCOMPLETE。[宿主证据](artifacts/dex-metadata-trace-20261101/stream-v2-host.json)不代表真实产品、完整查询、设备或性能资格；真实 DEX 轮次另行记录。
- 流式提交 `709f1d3` 的 [Windows/Linux CI](artifacts/dex-metadata-trace-20261101/stream-v2-ci-709f1d3.json) 均通过生成器、采集器、回放、控制器和 recorder 合同；这不替代实际设备与性能验证。
- 此前授权的真实 DEX 完成一次[宿主 V2 流式采集和合成回放](artifacts/dex-metadata-trace-20261101/real-dex-stream-v2.json)：四种策略与五场景 20/20 对、正常/跨线程场景各 6113916 次选定 reader 读取；100 份私有输出的大小、摘要和记录的子进程退出状态经独立回读。10 个 System/Arena 合成域正常关闭归零，另 10 个普通路径只确认引用清除。该证据不涵盖完整查询、原始并发寿命、Android 设备、BIL 产品集成或性能收益。
- 新增显式启用的[图像源码级采集与回放](consumers/image/TRACE_CONTRACT.md)：覆盖实际 P6 图像算法的 direct、System 和 Arena 路径及 full/ring 工作区。轨迹只记录相对访问与生命周期，不包含像素或原始地址；完整输入、原始轨迹和输出留在本机。
- 安装版宿主核心上的合成 P6 功能验证通过采集 6 正例、4 反例及回放 12 正例、53 反例；本机测试环境未能创建符号链接，但后续 Windows/Linux CI 的该拒绝路径均通过。[本机证据](artifacts/image-trace-20261101/host.json)、[公开 CI](artifacts/image-trace-20261101/ci-host.json)；不插桩目标不运行记录逻辑。
- 尚未取得用户业务输入或 BIL 产品访问轨迹；源码语义回放不证明 CPU 指令顺序、硬件机制或性能收益。输入文件原始 header 和实际加载映像身份也未认证，详见合同。
- 新增[宿主工程候选构建器](docs/HOST_CANDIDATE_BUILDER.md)，从独立源码快照构建核心、宿主 JNI 与纯 JVM 类；Windows 归档回读和真实图像消费者通过，21 项离线合同检查通过。基于 `4aa2874` 的旧 Windows 候选有一条原始日志被同名步骤覆盖，不能声称日志链完整；该轮保留，修复后的候选见上节。此时 Linux 分支只有离线合同检查，后续 Linux 构建证据见文件顶部。
- `4aa2874` 的 [Windows/Linux 公开 CI](artifacts/delivery-20261101/ci-host-4aa.json) 均通过；[该提交两份本地候选的回读记录](artifacts/delivery-20261101/prior-candidates-4aa.json)分别保留 Android 日志完整和 Windows 单条日志覆盖的差异。CI 不包含后续日志保护修复。

## 多域诊断与 Android 接入示例（2026-11-01）

- 新增 [MM-021 多域 Arena 诊断](bench/TLB_MULTI_ARENA.md)，以相同工作量比较两种访问顺序，检查预算、持有 lease 时关闭及最后归零；Windows 宿主 5 个正例与 8 个拒绝路径通过。它是单线程功能工具，不证明 TLB 机制或性能收益。[限定证据](artifacts/multi-arena-20261101/host.json)
- 新增[独立 Android 使用示例](consumers/android_managed/README.md)，显式处理后台注册、合成压力票据、注销与关闭；宿主 JNI 和 framework 类型桩通过，并用本批 arm64 AAR 与真实 Android SDK 编译通过。ART、真机和 R8 运行仍须分别验证。
- 新增[Android 工程候选构建器](docs/ANDROID_CANDIDATE_BUILDER.md)，该阶段从 `c45723f` 的独立源码快照构建并检查三 ABI AAR 与 SDK；14 项离线合同检查通过，独立消费者的三 ABI 交叉编译、链接及 AAR/SDK 身份回读通过。[历史候选证据](artifacts/android-candidate-20261101/candidate.json)；该候选早于后续修复，新版见上节。
- 本批 [Windows/Linux 公开 CI](artifacts/android-candidate-20261101/ci-host.json) 均通过：各自核心 39 项、托管域 49 项、多域诊断 5 正例和 8 反例，以及候选工具与 Android 示例的宿主检查。

## 托管域同域压力继续开发（2026-11-01）

- 新增显式选用的同域压力绑定：手动预算和压力事件由同一控制器仲裁；事件排队、应用、失败、取消与过期分开观察。关闭先结束压力控制器，再清理托管域；已有租约不被强制释放。Android 回调桥提供拒绝计数和最近状态，但回调路径会取短锁，不承诺硬实时。见[压力合同](docs/ANDROID_MANAGED_PRESSURE_CONTRACT.md)。
- Windows 宿主验证覆盖核心压力合同、托管绑定并发和关闭、确定性取消，以及图像消费者的 System/Arena 合成压力场景；[公开 CI](artifacts/managed-pressure-20261101/ci-host.json) 的 Windows 与 Linux 均通过核心 39 项、托管域 49 项及图像默认 12／压力 12／失败 10 组。详细源码和范围见[本次记录](artifacts/managed-pressure-20261101/host.json)。这些结果不证明 Android ART、R8 运行、真实系统低内存、设备长稳或性能收益。
- 之前的预览包和历史设备证据均不包含本次绑定；上节的新工程候选包含该绑定，但尚未取得设备或正式发布资格。设备和性能资格留待功能阶段结束后统一验证；BIL 保持未接入。

## Java 托管域上一开发阶段（2026-11-01）

- 新增纯 Java 可调用的 `ManagedMemoryDomain`、allocation 和受控 lease；自有 System/Arena 域分配时先清零，显式软预算，关闭先停止准入并在后台等待 owner、lease 和相关调用。未知结果进入隔离，不强制释放；该阶段尚未接入同域压力 worker，后续进展见上节。[托管合同](docs/ANDROID_MANAGED_DOMAIN.md)和[压力接入合同](docs/ANDROID_MANAGED_PRESSURE_CONTRACT.md)。
- Windows 与 Linux CI 均通过真实宿主 JVM 的 46 项受控故障/并发检查及 1 项无注入检查；独立 Java 图像消费者迁移到托管 API 后，两端安装共享核心与宿主 JNI 的 12 组像素对照、8 组拒绝及无诊断 sidecar 普通路径通过。[证据范围](artifacts/managed-domain-20261101/README.md)
- 新源码已完成三 ABI 交叉构建及独立脱敏候选，arm64 R8 名称/签名静态检查通过；原始 AAR 未脱敏且不可分享。当时最新接口的 R8 运行、Android ART、真机、性能和正式发布资格均待统一验证；后续限定 arm64 真机短测见本文件顶部。此前预览包不包含托管域和本次消费者迁移。

## 后续功能继续开发（2026-11-01）

- C++ RAII 新增 checked 外部导入及粘性隔离；释放结果不确定时不重试或销毁原域，保留的资源不能计为正常归零。[合同](docs/CPP_CHECKED_IMPORT.md)，限定宿主检查10项通过。
- Java 受控借用的首次关闭不再在 Java monitor 内执行原生释放，并提供非等待的关闭状态观测；宿主 fake-JNI/真实 JVM 检查通过。状态仅描述本注册，不能替代 domain 归零证据。[合同](docs/ANDROID_NATIVE_BORROW.md)
- 新增独立 Java/JNI P6→P5 图像消费者，使用安装后的共享核心和单独构建的宿主 JNI；12组像素对照及8组失败路径通过。Java 数组和 JNI 复制成本单独记录，不宣称性能收益。[消费者](consumers/java_image/README.md)
- 上一份 `Feature-Preview-20261101.zip` 不包含本节后续源码；该后续阶段的三 ABI Android 和 Windows 宿主候选已重建，但不包含之后新增的 Java 托管域。arm64 R8 诊断及关闭状态 v2 用例已有[构建和宿主模拟记录](artifacts/followup-delivery-20261101/android-borrow-v2/host-receipt-final.json)，故障域按合同保留，尚未执行 ART/R8 运行、设备、性能或发布资格。宿主证据见[记录](artifacts/continuation-next-20261101/host-dev-checks.json)，Android 构建身份见[记录](artifacts/continuation-next-20261101/android-build/metadata-index.json)，BIL 仍未接入。
- 仓库尚未提供项目许可证文件；既有 LLVM notice 已保留，正式发布前的项目授权条款仍需确定。
- 交付候选后的 CI 发现 Windows 检出字节漂移和历史多命令步骤掩盖失败；修复后的 Windows/Linux CI 已分别通过。旧 Linux 宿主桩标准错误未被保存、具体内容仍未知。结果见[跨平台记录](docs/CI_BYTE_IDENTITY_20261101.md)，既有预览包和旧回执未改写。

## 功能继续开发（2026-11-01）

- 外部导入增加可报告 origin 释放失败的可选扩展；失败时保留不确定 claim 和账目，旧 ABI 版本值不变。旧接口及新接口各自的地址复用边界见[导入合同](docs/EXTERNAL_IMPORT.md)。
- 新增无逃逸指针的 Java 原生 lease 字节访问、受限暂存和独立 Android 测试入口，见[接口合同](docs/ANDROID_NATIVE_BORROW.md)与[设备协议](docs/ANDROID_BORROW_DEVICE_PROTOCOL.md)。宿主开发检查、设备运行器离线回归和脱敏版独立 R8 测试 APK 构建完成；ART、R8 运行和设备验证尚未执行。[构建记录](artifacts/feature-development-20261101/android-borrow-r8-build.json)。
- 新增可选 Compute CPU READ 桥，映射和独立 lease 由原 owner 保活，组件独立构建，不改变核心或 BIL 的依赖图。以最终核心和真实 CPU reference SDK 的宿主检查4项通过，独立安装消费者的正常与故障隔离路径通过；Android/GPU 和性能资格仍待统一验证，详见[桥合同](adapters/compute/README.md)。
- 已核实 Lumen 当前软件工作区及压力接口；现有公开 API 不接受外部 allocator，未改动或接入 Lumen。[合同记录](docs/LUMEN_MEMORY_CONTRACTS.md)区分实际代码、可选协调方向和未验证事项。
- Android arm64、x86_64、armeabi-v7a 核心、JNI 与单 ABI AAR 已交叉构建，并生成独立脱敏候选；加载段、导出、依赖及静态库相关段和重定位核对见[记录](artifacts/feature-development-20261101/android-sanitized-v2/manifest.json)。这不等于三种 ABI 均已在设备运行。16 KB、MTE/HWASan、长期稳定与性能验证仍按原范围单列。
- 脱敏工程预览包及内容哈希见 [ARTIFACTS.json](ARTIFACTS.json)；它未绑定最终提交，也不代表设备或正式发布资格。

## 当前阶段

API33停滞已在独立复现中观察到目标自身cgroup冻结，触发者及历史每次是否同因未知。相同APK后台再次冻结；另一次在instrumentation内启动前台Activity时，[Activity尚未resumed即观察到冻结](artifacts/android-close-investigation-20261030/freezer-control-foreground/receipt.json)。先由shell预启动受保护诊断窗口、再以no-restart保持同一进程的组合，短对照完成1000周期且采样未见冻结。两种前台启动方式和重启时序不同，不构成单变量因果结论。JNI的独立自动通知及错误分支缺口已修复，Host Release/ASan各29项通过。

设备环境补充：用户说明手机安装了相关冻结模块并导致上述冻结。这是用户提供的信息；原始采集只记录了进程冻结，未独立识别每次冻结的触发者，历史回执保持不变。

最终去debug路径的安装版在API33 arm64真机独立运行 901.089 秒、14959 周期，366次自身cgroup采样未见冻结；窗口正常结束，预算、跨关闭lease、无调用者poll自动收尾、未知原因及正常域归零通过。API34 x86_64独立模拟器的同版短测通过，未建立API34长稳。BIL未接入，性能未测。详见[关闭调查与验证](docs/ANDROID_CLOSE_INVESTIGATION_20261030.md)。

## 当前已交付

- 核心：System/custom provider、有界 arena、唯一 backing 和 reservation 预算、owner/读写 lease、非阻塞 begin_close、collect/drain、reset/trim、C++ RAII、CMake install/export。
- 0.2 新能力：SYSTEM 完整外部 region 导入、runtime 路由和重叠拒绝、软预算；Android pressure worker、ComponentCallbacks2/JNI 异步关闭及自身 RSS/PSS/private-dirty；独立 packed reader。
- 0.2 首阶段功能证据：Host Release/ASan 各10项；packed Java32、真实 Kotlin oracle288、ART32；API33 arm64/4KB 设备原生9项、Android20项及针对自身包的 AMS 注入通过。详情见 [0.2阶段收尾](docs/FEATURE_STAGE_20261030.md)。该阶段仅有上述限定证据，不单独构成后续长期稳定性、资源节省、R8或产品资格。
- 真实消费者：安装后的 Memory SDK 已由冻结 DexMeguviz 源码和 P6 文件图像工作区消费。v2 在 API33 arm64/4KB 实际通过 core3 和消费者196项（DEX32、图像工作区156、图像计时合同8）。

## 历史实验与归因

真实 v2 采集器已经实现并执行；四组制品和协议在采集前冻结。pilot128任务完成；图像因精度门槛未进入formal，DEX formal完成4/12窗口、64次native后因环境门槛停止。设备窗口已结束，没有排队任务；不完整正式区间已撤销，见 [v2最终结果](docs/DEVICE_V2_RESULTS.md)。不得继续把这一轮称为“离线工具、尚无采集器、只交叉构建”或“pilot仍在运行”。

更早的独立 DEX A1/A3 实验完成32块/128任务，但存在趋势警告，性能为 INCONCLUSIVE。新旧轮次不混池；见 [历史性能报告](docs/PERFORMANCE_A1_A3.md)。

普通直接借用删除一份10,540,184B输入及复制；1080p三行环少2,067,840B payload，当前arena chunk下少2,008,064B backing。以上是普通消费者优化，不是 Memory 增量或 RSS 结论。已有BIL scratch/holder/staging复用不重复计功。自身 RSS/PSS 已能观测，但尚无同完成量资源净收益；能量没有合格方法。

## 上一阶段历史

本阶段按用户要求快速收尾：DEX外部导入、图像压力/预算/异步关闭组合及Host/arm64原生900秒循环通过；最终JNI锁修复Release/ASan各18项、8组原/新反例及双ABI真实R8短测通过。Android长稳没有取得资格：首个API33子项1260秒超时，closefix新轮仍停在iteration48/cycle.idempotent-close，缺根因栈；新API34轮仍有进度，但按用户快速收尾要求提前结束。旧模块API34完整900秒结果独立保留，不提升新模块资格。仅结束本项目诊断进程，不清应用数据、不停止其他会话。详情见[阶段交付与阻塞](docs/CONSUMER_CLOSEOUT_20261030.md)。

## 剩余边界

- 16KB真实loader、MTE/HWASan进程及故障试验、同完成量能源、正式性能和发布资格独立缺失。
- 本阶段新增0.2 x86_64 SDK及派生closefix1压力AAR，API34 R8短测通过；完整矩阵不从旧0.1升级，armv7未重新覆盖。
- Compute/Media真实SDK及合同已核实，Compute公开host descriptor没有外部CPU指针/provider注入口；反向完整CPU READ-map桥已实现独立宿主路径，Android、产品和性能资格未取得。见[家族合同](docs/FAMILY_MEMORY_CONTRACTS.md)和[桥合同](adapters/compute/README.md)。Lumen/Bitmap/HWUI未接入。
- 压力适配JNI已有；通用ByteBuffer借用API、Rust、slab、Tile Heap、特殊placement/TLB策略未选择，不将其作为基础CPU库必需功能。
- BIL保持原实现，本轮不改其依赖或默认开关；无生产热点、接线及稳定净收益证据不宣称产品资格。

当前能力见[CAPABILITY_MATRIX](CAPABILITY_MATRIX.json)，MM任务与原依赖见[TASKS](TASKS.json)。原0.1与0.2收据、制品清单均保留；新继续阶段证据独立放入 `artifacts/continuation-20261030/`，不改历史原始实验。
