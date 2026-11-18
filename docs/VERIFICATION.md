# 2026-10-30 工程验证回执

本批交付 CPU 核心、C/C++ API、安装包及两个真实可运行消费者。完整任务维度见 `TASKS.json`，实际源码和二进制哈希分别由 `SOURCE_LOCK.json`、`ARTIFACTS.json` 固定。

| 证据 | 实际结果 | 适用限制 |
|---|---|---|
| Windows x64 / MSVC 19.51.36247 / Release | CTest 4/4 PASS | core 11 组、145,033 check；纯 C、C++ RAII 5 case、两个独立 DLL 句柄隔离 |
| Windows x64 / MSVC AddressSanitizer | CTest 4/4 PASS | core、C、C++ 编译插桩；独立 runtime 测试验证身份，不将未插桩的辅助 DLL 算作完整 ASan 覆盖 |
| Windows x64 静态 SDK | 安装后图像消费 22/22 PASS | 独立 find_package 工程链接静态 mgmem.lib；没有依赖另一份 Memory DLL |
| GitHub Windows/Linux | 各 CTest 4/4、安装后图像 22/22 PASS | 代码提交 `58ed07f54199a1217ddef47735d4d297357654f4`；[构建 #40995571678](https://github.com/jichuo1/MeguvizMemory/actions/runs/40995571678)；没有冒充 CI 的 Dex SDK 验证 |
| 安装后的 DexMeguviz 消费者 | System/arena PASS | 真实 borrowed snapshot/query，完整消费两方法结果；与 copy-in oracle 相等；result 跨 snapshot/owner close 保活；最终两个库 borrowed/backing 归零 |
| 安装后的图像工作区 | 10 正例、12 错误路径 PASS | 五种尺寸×两模式；独立 Python luma/blur oracle 逐字节相等；截断/溢出/错误 header/不可写输出均失败退出 |
| Android NDK 30 / Clang 21 / API 27 基线 | x86_64、arm64-v8a、armeabi-v7a 构建安装成功 | 未削减 32 位；全部使用 static C++，实际 `libmgmem.so` 仅依赖系统 libm/libdl/libc |
| 最终 Android ELF | 三 ABI 的所有 PT_LOAD 对齐均 0x4000 | 静态 ELF 条件通过，不表示真实 16 KB loader 或 APK/AAR 打包通过 |
| API 34 x86_64 / 实际 4096 B 页模拟器 | core 11 组、C ABI、C++ RAII 实际运行 PASS | 独立 `/data/local/tmp` 目录真实动态装载；未运行 BIL/LSPosed |
| owned-region diagnostic | Windows 48/48、Android 模拟器单 case checksum PASS | HOST_DIAGNOSTIC；不作为性能/TLB 机制资格 |
| 研究模型重算 | 三个 calculator 与原 JSON 语义相同 | MODEL 算术复现，未测设备收益 |

真机只读核到了 API 37、arm64、4096 B 页、6.12 Android 内核，另一会话正在做 GPU 工作，本批未在该真机执行 Memory。没有可用的真实 16 KB 环境；MTE/HWASan/CheckJNI、arm64/armv7 实机、产品端到端延迟、能量和 BIL 集成都未验收。

## 实际复现入口

宿主 CMake/build/install 步骤见 README 与 INTEGRATION。构建 owned-region 诊断时需显式设置 `-DMGMEM_BUILD_BENCHMARKS=ON`。当前机器的实际目录为 `build/host-ninja`、`build/host-asan`、`install/host`、`build/dex-locked`、`build/image`。Python 运行既有解释器，没有安装 Python 依赖：

```powershell
$env:PATH = "$PWD/install/host/bin;" + $env:PATH
python tools/verify_consumers.py --image-exe build/image/mgmem_image.exe --dex-exe build/dex-locked/mgmem_dex_consumer.exe --dex-fixture build/locked-inputs/DexMeguviz/tests/fixtures/generated/minimal.dex --out-dir artifacts/verification-20261030/consumers
install/host/bin/mgmem_tlb_owned.exe --matrix
```

Android 使用现有 NDK `30.0.16248370` 的 `build/cmake/android.toolchain.cmake`，`ANDROID_PLATFORM=android-27`、`ANDROID_STL=c++_static`、各自 ABI 与独立二进制/安装目录。先构建安装，再运行 `llvm-readelf -l -d install/android-<abi>/lib/libmgmem.so`。模拟器明确设置 `LD_LIBRARY_PATH` 为仅含本批库的临时目录，传入同批 core/C/C++ 测试 ELF；raw 结果保存为 `artifacts/verification-20261030/android-api34-*`。

测试覆盖真实风险：并发 reservation/close 竞争、provider OOM、异常不越过 C ABI、精确地址/容量/对齐归还、旧数据失败原子、读写互斥、跨域/重复释放、晚完成 worker、reset/trim 拒绝活对象、增长共存峰值、阻塞 deallocate 时 drain 真超时、双 runtime cookie 隔离。最终 destroy 与全部 API 仍由外层串行化，裸 domain 指针不提供销毁后安全。

## 经验与失败记录

1. 初次 Visual Studio 生成器未找到编译器。原因是当前终端未初始化 MSVC；使用可覆盖 VS 根目录的 `tools/host-build.cmd` 初始化现有工具链，Ninja 构建成功，没有安装新工具。
2. 首轮 ASan 测试启动超时。原因是直接运行 CTest 时缺少 MSVC ASan runtime DLL 搜索路径；给当前进程 PATH 加入编译器 Hostx64/x64 目录后 4/4 通过。保留这个运行环境前提，不把该超时隐去或当核心通过。
3. 审查发现 drain 曾同步执行自定义 provider 释放，因此 0ms 可能阻塞。最终方案拆出 cleanup worker 的 collect，drain 只等待；阻塞回调的确定性 CV 测试已通过。
4. 审查发现独立 core 模块的局部 domain ID 可相同。最终 handle 增加装载期 runtime_origin 路由 cookie，两个实际 DLL 的交叉释放返回 STALE_HANDLE；runtime 卸载后句柄仍禁止复用。
5. 观察到 arena 为小输入保留更多容量。保留原始结果，默认 System 仍是有效基线，不选择特殊布局，不将调用次数变化写成时长/RSS 收益。
6. Android 最终重传后测试 ELF 的执行位被重置，首次返回 Permission denied；给本批独立目录的测试文件恢复执行位后重新运行全部 core/C/C++，并核验设备上 libmgmem.so 的 SHA256 等于最终安装制品。该错误没有被计为通过。
7. 外部 Dex 会话在最终核对后更新 CMake/任务元数据。没有将漂移静默记为哈希通过；最终复制独立配置输入和源码快照，重新构建 parser 与安装消费者并重跑 24 条 oracle 命令。旧回执保留于前一证据提交，最终 SOURCE_LOCK、ARTIFACTS 与回放输入 ZIP 对应冻结版本。

本批没有修改外部 BIL/Dex/Compute/Media 源码或依赖版本，也没有正式发布 Release。公开仓库用于保存工程预览及进度，CPU 正式候选、家族集成和 TLB profile 各自仍需对应门槛。

## 后续批次：arm64 真机安全（同日）

原设备 owner 明确释放后，根代理重查诊断进程和 instrumentation 均为空，使用新建的独立设备目录运行本批 `arm64-v8a` 安装库及 core/C/C++ 程序。设备 PMA110、API 37、内核 6.12 Android、基础页 4096 B；四份上传 ELF 均逐个校验 SHA256，boot identity 前后相同。核心 11 组、145033 项检查，纯 C 最终 backing=0，C++ 五个 lifetime case 全通过。用例已包含 lease 迟到、六线程竞争、预算 reservation、失败原子、reset/trim BUSY、关闭/申请竞争及阻塞回收时 drain 超时。

证据在 `artifacts/arm64-20261030/safety/receipt.json`；脚本为 `tools/run_arm64_validation.py`。设备库存原文仅在内存解析，公开数据只保留摘要和相关诊断行，不发布无关应用/页面。该批是普通 arm64 功能验证，不包含 MTE/HWASan、16 KB loader、BIL 安装/Hook 或性能通过。后续 A1/A3 性能协议及结果独立保存，不把安全程序耗时作为收益。

## 后续批次：独立真实 DEX 的 A1/A3

实际源为本地 BIL 9.17.0 APK 的 `classes.dex`，10,540,184 B；两组完整处理 54,147 个方法，摘要一致。64 MiB parser 预算的预检失败保留后，在任何 pilot 前将两组 parser 预算共同固定为 256 MiB，Memory 输入域仍为 32 MiB。A1 直接 System 单 backing，A3 安装后的 arena；完整 setup/parse/query/consume/cleanup 均计时，文件预载与进程启动不计入 CPU 主指标。

首轮 96 块计划在第 36 块末遇 Windows 收据替换访问冲突，144 次 native 均成功，但末次环境回执缺失；35 个有效块和全部失败记录独立保留，判 INCONCLUSIVE。修复为最多 0.9 秒有界重试及追加命令 journal，真实 Windows 短/长读句柄冲突和日志恢复回归通过，并已进入 Windows CI。

唯一一次独立重试重新 pilot 后固定 32 块，128 次任务、环境和关闭检查完整通过；配对几何时间比的描述性净增量 1.0793899%，探索性 95% 区间 −0.0234357%～2.3315899%。前后半程 log-ratio 偏移超过预设 0.01，最终仍 INCONCLUSIVE；不混池、不追加 pilot、不接入 BIL。两组 backing 峰值均为 10,540,184 B。详情与原始入口见 `docs/PERFORMANCE_A1_A3.md`、`docs/BIL_ADOPTION_DECISION.md`；设备窗口已结束，后续只在主机归档。

## 后续批次：Pack 受保护 producer fence 协调桥（2026-11-17）

Pack 侧 `bbbf529`（MeguvizCompute HEAD）合入 22 文件的 guarded producer fence 协调桥，Memory 侧只读验证其可选 guarded 交接；本批未改 Memory 源码，未提交。运行 `20261117-170044-4cf676a1` 的 65 步回执全部通过，Vulkan 未跳过。

| 证据 | 实际结果 | 适用限制 |
|---|---|---|
| host-pack / host-runtime / host-gles / host-integration | CTest 9/9、20/20、24/24、4/4 PASS | 宿主自定义 harness 与 Pack 自带测试；`MGC_GUARDED_PRODUCER_FENCE_EXPERIMENTAL=ON` |
| arm64-v8a、armeabi-v7a CPU 与 arm64-v8a Vulkan | 交叉编译、安装和 C/C++ 消费者链接通过 | 只做交叉编译/链接，未在设备、模拟器或 GPU 上执行 |
| guarded Vulkan 复编 ON/OFF | 两次均 PASS_CROSS_LINKED | 复用冻结的 ncnn/glslang SDK，不是完整依赖重建 |
| 旧 SDK 普通路径 | 默认构建与消费者链接通过 | guarded 配置在缺新符号时按设计 fail-closed（`CMake Error`，未被计为通过） |
| Android 编译期静态布局 | AhbAllocator +264→+40 B、SessionIdentity +95→+3 B；resource/session/ticket 0 | ELF 符号长度差，不是 RSS、分配器可用尺寸或驱动内存 |
| host 配对唤醒 | P50 中位增量 2.0 µs（fence 优先）/0.95 µs（cleanup 优先），均 8/8 对更慢；P95 混合 | 合成 fence 状态，实际 Pack worker；不覆盖 Android eventfd/poll 路径 |
| host 配对生命周期 | P50 候选快约 11.1 µs，8 对中 2 对慢；new 次数 0 增量，请求字节每批 −3584 B | 8 session/16 resource 的 64 B 负载；不计 malloc、线程栈或 RSS |

执行源码与提交一致性另行核对：`bbbf529` 的 22 个文件与冻结测试快照逐字节相同（3 个 `.inc` 仅行尾 CRLF 差异），`apply-manifest-final.json` 的全部 `testedSha256` 在快照上复现。宿主为 MSVC 19.51.36247.0 / Release，构建工具为 Android SDK 自带 CMake 4.1.2 与 Ninja，没有安装新依赖。

原始回执在本地忽略目录 `build/pack-fence-opt2-20261117/final-evidence-r2.json`（`state=PASS`，3866 个文件哈希）、`apply-manifest-final.json`、`review-final.patch`，运行日志在 `build/guarded-fence-validation-20261117/runs/20261117-170044-4cf676a1`。该路径未进入公开产物；本批没有设备执行、GPU 驱动计时、RSS、能耗或产品端到端结论，也不构成 Android 性能资格。
