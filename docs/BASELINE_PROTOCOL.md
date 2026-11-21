# 基线与验证协议

本轮完成了 CPU 功能实现与安装消费方验证：宿主 Release 与 ASan 均通过四项 CTest；Android API 34 x86_64 模拟器的真实 4 KiB loader 上运行通过核心、纯 C 与 C++ 用例；图像与 DEX 两个独立消费方也通过安装后的 Memory API 跑通实际工作负载。

协议编号 `MM-003-host-functional-diagnostic-v1`，冻结日期 `2026-10-30`（Asia/Shanghai），机器可读入口为 `bench/protocol.json`。`MM-003.protocol_ready=READY` 仅表明宿主功能与诊断协议可运行；完整产品 baseline 仍为 `BLOCKED`，不可将此标记视为 `MM-003` 整项完成。

## 固定输入与对照

历史 BIL 研究基线为 `74d78e19095baa006b1315001190241952f1b84a`，只读审计的 BIL main 为 `00572940c354fbec7b6f311d4ecd6f02e66ea7c5`（保留原有用户修改与其他 worktree）。由于 `scratch` 复用已存在于 P1-07 历史提交 `e630ad56`，原有的五槽 direct buffer、有效范围初始化与五次取第二快统计不可重复记作 Memory 净收益；所审计的 BIL main 中也尚未接入 packed reader，本批仅围绕 CPU 库与两个安装消费方展开，未修改 BIL。

| 版本 | 实验合同 | 本轮状态 |
| --- | --- | --- |
| A0 | 固定历史消费方源码、制品、工作量与成功条件 | 仅识别历史来源，无本轮产品采样 |
| A1 | 常规优化后的既有 System 路径 | 已核 BIL scratch 源码，无本轮配对性能样本 |
| A2 | 保持相同 native provider/backing、初始化、owner、布局与工作量，改走 Memory System API | 当前两个消费方的 System 模式只用于功能对照，尚未构成与 A1 对齐的纯接口性能实验 |
| A3 | 在 A1 基础上引入 Memory region/arena 并计入接入开销 | 两个消费方 Arena 模式功能通过，无产品净增量资格 |
| A4 | 在 A3 基础上引入取得设备与消费方资格的 placement | `NOT_SELECTED` |

如果将 Java direct buffer 迁入原生时一并改变了清零、复制或存储持有方，则 `A2 - A1` 只能记作“迁移与接入总差”。全方案、Memory、核心和 placement 收益分母分别为 `(A0 - A4) / A0`、`(A1 - A4) / A1`、`(A1 - A3) / A1`、`(A3 - A4) / A3`；缺少对应版本时不填写结果，不同分母的百分比不可相加，接入开销也不重复扣除。研究模型中的 `3%`–`5%` 为候选目标，`3.55%`、`0.20%`、`11.50%`、`−0.35%` 为条件推算值，本轮未将它们填入实测栏位 `LOCAL_MEASURED`；三份算术重算与原输出一致仅证明可复算。

## 本轮可重现的功能场景

| 场景 | 固定工作量与校验基准 | 已执行证据 |
| --- | --- | --- |
| 核心 owner/lease/预算/arena | 11 组用例共 145,033 次检查：失败原子性、原 provider 配对、扩容共存峰值、关闭竞争、迟到 lease、`reset`/`trim` 与并发压力 | 宿主 Release 与 ASan；API 34 x86_64 4 KiB 模拟器 |
| C ABI | 纯 C 程序完成真实申请、读写与释放，关闭时 backing 归零 | 宿主、同一 Android 模拟器 |
| C++ 窄适配 | `include/mgmem/memory.hpp` 5 个 RAII 与失败回退用例 | 宿主 Release/ASan、同一 Android 模拟器 |
| 双 runtime 来源 | 同时装载两份核心动态库，拒绝交叉传递句柄 | 宿主功能通过（非最终家族单 DSO 闭包证明） |
| PPM 图像工作区 | 固定 RGB P6 输入，整数灰度与边缘夹取 3×3 均值模糊，输出与 Python oracle 逐字节比对 | 5 种尺寸 × 2 模式共 10 个正例通过；10 个非法输入与 2 个不可写路径按预期失败 |
| DexMeguviz 解析器 | 真实 `minimal.dex`，`dmg_snapshot_open_borrowed` 与查询对照独立 copy-in oracle，结果跨 snapshot 保活 | System 与 Arena 两模式均 PASS，source SHA、方法数与结果校验一致 |
| 安全 owned-region 诊断 | 真实页、固定节点/访问次数、相关/无关访问、页内 rotate/fixed 与固定种子校验和 | 宿主 48/48；模拟器单用例通过 |

安装消费方统一验证入口：

```text
python tools/verify_consumers.py --image-exe <installed-package-linked-image-exe> --dex-exe <installed-package-linked-dex-exe> --dex-fixture <real-minimal.dex> --out-dir <receipt-dir>
```

图像输入固定为 `1×1`、`1×7`、`7×1`、`17×13`、`320×180`，RGB 生成规则与 oracle 内置在工具脚本中。每条命令带超时控制并记录 `stdout`/`stderr`、退出码与输入输出 SHA-256。完整收据 `artifacts/verification-20261030/consumers/receipt.json` 为 24/24 PASS；`--image-only` 分支为 22/22 PASS，不计入 Dex 资格。工具总耗时包含进程启动与校验时间，不作性能样本。

Dex 消费方需从外部只读源码构建（`tools/source_lock.py` 须同时锁定 `include`、`src`、`third_party`、`CMake` 等全部构建输入），最终收据必须关联源码、安装库与可执行文件的哈希。

## 正式性能采样的前置条件

当前状态为 `INCOMPLETE_FOR_PERFORMANCE`。开展正式采集前，须先固定真实主场景、版本/制品/输入输出哈希、画质与档位、调用次数及初始化范围；完成独立 pilot 后再冻结样本量、主指标、配对均衡顺序、时间与自有 backing 回归预算，以及温度/迁核/后台负载的数值剔除阈值。

统计单位必须为独立 session 或预定义时间块（单帧、循环内的 load 或同一 session 内重复运行不是独立样本）。诊断轮与低扰动计时轮须分离，并保留全部慢样本、失败、超时和回退数据，不得只取最快样本，样本不足时也不报告 p95/p99。多线程 CPU time 须单独列出而不可相加充当关键路径 wall time；申请次数、累计 requested、唯一 backing、`live`/`retained`/`retiring`、RSS/PSS、缺页、触页、padding、迁核、温度及同完成量能耗分别记录，未知项记为 `null`/`UNAVAILABLE` 而非 `0`。

## 适用范围与验证说明

- **设备与打包边界**：Android `x86_64`、`arm64-v8a`、`armeabi-v7a` 均已完成核心与 C++ 头的交叉编译安装，三个核心 ELF 的 `PT_LOAD` 对齐为 `0x4000`、`DT_NEEDED` 仅含 `libm.so`、`libdl.so` 与 `libc.so`（静态链接 C++ runtime）；但仅在 API 34 x86_64 4 KiB 模拟器实际运行，尚未完成真实 16 KiB 页加载器、`arm64`/`armv7` 全量实机性能、MTE、家族桥全量组合、能耗以及 BIL/LSPosed 宿主验证。
- **ASan 运行记录**：首轮 ASan 因运行时诊断 DLL 未加入 `PATH` 而超时，修正环境变量后 4 项 CTest 全部通过；ASan 与 Release 数据不混作性能比较。原始日志与 ELF 检查记录见 `artifacts/verification-20261030/`。
- **TLB 与退出条件**：本轮未产出 placement/hint 发布 profile，PMU、PFN、物理连续性与硬件聚合均未取得资格（详见 `bench/TLB_QUALIFICATION.json`）；调整步长、VA 对齐或 ELF 对齐不会改变 OS 基础页。当前 `candidate_decision=INCONCLUSIVE`；若无稳定净收益则维持 System/dense，回退时只将新建域切回合格路径，让存量域按原路径自然完成 `collect`/`drain`，严禁搬迁稳定地址或按超时强制释放。本工程预览与宿主检查均不构成 `MM-019` 正式发布资格。
