# Android 托管域和合成压力真机诊断协议

本协议只针对 `io.github.jichuo1.meguviz.memory.managedprobe` 和其 `ManagedInstrumentation`。使用当前候选 AAR 的托管域在 System、Arena 及合成回调下检查预算、受控租约和关闭；不会接入 BIL、清除数据、卸载软件、停止其他应用或改变系统冻结策略。报告仅限所选设备和本轮输入，不构成长稳、真实系统低内存、性能或产品消费者资格。

## 冻结

先单独运行 `python tools/test_android_managed_round.py`；它只使用合成制品和 fake 设备，不连接 ADB。借助托管诊断构建器产生 R8 版本的 APK、构建回执、对应 AAR 和安装 SDK core。`python tools/android_managed_round.py freeze --help` 列出全部显式输入：serial、预期 API 与基础页大小、APK、AAR、SDK core、构建回执、ADB、aapt2、Java、apksigner、时限，以及可选的全新计划路径。所有项目输入须是工作区内文件；默认将计划放在 ignored `build/managed-device-plans/` 下新建的独立目录，也可显式指定 `build/` 内全新的独立目录。为兼容旧流程，仍可指定 `artifacts/managed-device-20261101/` 下尚不存在的路径。冻结输出会给出本地计划路径和哈希。计划包含本机相对路径，属于私有资料，**无论位于哪里都不得上传或提交**。该步骤不会调用 ADB。

冻结器核验专用包及唯一 instrumentation 的 manifest、单一签名、APK 和 AAR 的两份原生库与 SDK core 字节、ELF 架构、classes.jar 和 DEX 哈希、R8 构建回执和源码身份，限定 APK 打包成员；制品、测试源码、构建器、采集器及离线测试按字节记录，外部工具也记录哈希。清单和源码哈希只绑定身份，不单独证明构建来源。冻结标准输出的 `plan_sha256` 必须在执行前独立保存。执行用 `run --help` 所列参数，提供完全相同的输入、计划、原先保存的 `--plan-sha256`、全新公开回执目录及 `build/` 下全新私有目录；执行前重新核对计划和全部输入的字节。不可通过修改计划后重新计算哈希冒充原冻结计划。

构建回执采用 `PASS_R8_BUILD_ONLY_NOT_DEVICE_EXECUTION`、`device_executed=false`，包含 package、instrumentation、ABI、APK 字节数与哈希、签名；`inputs` 包含 AAR、共享 core/JNI、classes.jar 的 SHA-256；`source_sha256` 绑定三个诊断源码、构建器、采集器及其三个共享构建辅助脚本；`r8_evidence` 绑定 dex 哈希和 instrumentation 保留、托管 native 签名保留、收缩见证被移除三项证据。这些证据不证明 ART 已执行。

## 准入、安装与执行

只接受显式 serial，不自动选择设备。运行前、安装前和启动 instrumentation 前分别读取并检查进程和 activity 清单；清单无法解析、相关进程或活动 instrumentation 存在时阻塞。关键词只覆盖已知名字，系统视图也可能有隐藏状态；三次检查均是时点观察，`shared_device_lock=false`。在共享设备上须另行协调使用窗口，并在真正执行前重新检查；观察通过不等于获得其他会话认可或排他锁。API、ABI、页大小和 boot ID 必须符合计划。

枚举所有可见 Android 用户及其该包安装和卸载残留。当前用户已有同包时须拉回 APK 并确认签名相同；若其他用户实际安装或存在数据/启动残留，禁止安装或更新共享代码，除非当前用户已安装的字节与本轮完全一致、无需更新。清单、路径、签名或残留状态不明确时阻塞；这些只读检查同样不是共享锁。无论安装还是复用，都须在启动前拉回 APK 并核对冻结字节。安装请求前先记录 `REQUEST_IN_FLIGHT_OUTCOME_UNKNOWN`；超时或不能回读时保持未知，不自动覆盖重试、卸载或清数据。

instrumentation 后台运行，不做前台预启动或特殊重启策略。采样只观察本包 PID、UID、启动身份及该进程 cgroup；检测到冻结、时间超限或进程身份变化为 INCOMPLETE。无样本写 `NOT_OBSERVED`，有未冻结样本也只是离散观察，不证明全程未冻结。采样跨过截止时间后才看到退出仍为超时。用户提到设备上安装了冻结模块，是否影响本轮以自身 cgroup 实测为准。

异常或超时仅终止本地 transport，绝不自动 force-stop；远端进程可能仍在运行，状态必须保持 UNKNOWN，不能把本地退出等同远端退出。异常后需只读确认远端状态；测试结束后若本包仍驻留，后验不通过，不强制停止以制造空闲。

## 结果和后验

成功报告必须为唯一一条严格 JSON，后接唯一 `INSTRUMENTATION_CODE: 0`，transport 退出码为零、标准错误为空；`suite=android-managed-pressure-v1`，检查数为 302 到 1000000 的整数（轮询可能增加次数），`case_count=3`，`final_backing_bytes=0`，`callback_origin=synthetic`，`queued_implies_applied=false`，`performance_qualified=false`。受理票据并不等于已应用；测试内部等待各有票据事件 APPLIED，并核对预算和最终账目。严格合法的 `FAIL` 报告与设备环境、超时或资料不全的 `INCOMPLETE` 分开。回调为测试中直接调用，即使通过也不能声称收到了系统低内存事件。

通过报告后再次核验无相关进程及活动 instrumentation、boot ID 与当前用户不变、已安装包路径和 APK 字节不变，后验全通过才能认定本轮 PASS；报告先通过而后验失败时保留报告并标记整轮 INCOMPLETE。测试源码没有前台保活和长稳协议，不能从短轮推导长稳。原始进程清单、命令输出和拉回 APK 只放新建 ignored `build/`；公开回执只记录范围、计数和哈希，不包含原始序列及本机路径。回执先 fsync pending、再有界原子替换；共享冲突保留此前完整文件和 pending，不覆盖历史证据。

## 多用户全局已知项与实际安装

`pm list packages -u` 的条目可能只表示全局已知包，不能当作每用户已安装。采集器保留该清单，并另取每用户不带 `-u` 的安装清单，再从本包唯一 Packages 段解析所有可见用户的状态；Queries 段的 User 行不参与判断。

他用户只有明确 `installed=false`、`ceDataInode=0`、`stopped=true`、`notLaunched=true` 且无 `-u` 清单一致的空状态不阻塞当前用户同签名更新。真实安装、数据残留、曾启动或未停止均继续阻塞共享代码更新；字段缺失/重复、格式未知或两个视图矛盾时整个准入失败。旧的完全相同APK免更新路径保持，不能据此安装或更新他用户共享代码。系统清单仍是离散观察，不是共享设备锁或所有隐藏数据的证明。

## 可选 copyTo 与 Arena 多块范围（2026-11-09）

默认 `--probe-scope pressure-v1` 保持旧三组压力报告合同。新版诊断另外支持显式 `--probe-scope copy-arena-v1`，freeze 和 run 必须使用相同范围；计划记录新 suite，不能把旧压力报告作为新功能通过。

新范围先执行原三组，再分别在 System/Arena 域执行 lease 直接传输、正反重叠自拷贝、owner 结束且域 CLOSING 后的持 lease 传输。Arena 分支还实际申请并释放96个同形64字节工作区，跨越两个4 KiB自有块，确认8192字节预算及拒绝新块，最后归零。它验证同形多块的功能，不证明查找指令、精确first-fit选址、外部Arena导入或性能；这些须由独立原生用例和配对测量验证。

新报告固定 `suite=android-managed-copy-arena-v1`、`case_count=5`、`copy_to_cases=2`、`arena_same_shape_allocations=96`，检查数至少1100并保留原有零账目、synthetic、非性能标记。构建回执必须同时确认 nativeTransfer 的 `(JJJJI)I` 签名在R8后保留，且诊断类的DEX字节码存在实际copyTo调用。构建检查不是ART执行。

构建器可选参数 `--diagnostic-keystore build/<已有本项目诊断目录>/diagnostic.keystore` 复用本项目旧 `managedprobe` 测试签名，默认仍在新build目录生成新诊断key。旧key只读使用，构建前后核对身份；不复制到公开制品、不输出私钥。安装前仍由runner拉回已安装包并匹配证书，签名不匹配不得靠卸载或清数据解决。

专用宿主桩新增 `android_managed_copy_arena_host_stub`，只证明该诊断代码在宿主JNI与framework桩中的功能；R8 arm64 APK和真实设备报告分别记录。
