# 独立 DEX 外部输入导入

本模块使用安装后的 Memory 0.2.0 / ABI 1.1 和既有冻结 DexMeguviz 源码。新目标为 `mgmem_dex_import_consumer`，不修改原 `paired.cpp` / `paired_v2.cpp`、原协议或历史采样。它不是 BIL 的 DexKit/Hook 路径，也没有向 BIL 添加依赖。

## 输入与所有权

两组都由 intrusive origin owner 持有真实 DEX 的 immutable `vector<uint8_t>`。文件长度作为 parser 输入长度，实际 vector capacity 作为完整 backing 容量；发布后不修改或移动 vector/context。

- `direct`：普通消费者直接借用 origin 的输入。SDK 回调保活独立 bridge；最后 SDK 引用结束才释放 origin。
- `import`：SYSTEM 域按完整 capacity 导入相同 origin，显式绑定当前 `mgmem_runtime_origin_v1()`，访问权限为 READ。成功导入取得 origin 引用后，bridge 获得 READ lease，并归还 loader 的初始 origin 引用。SDK 借用该 lease 的原地址，不建立额外输入 buffer、不复制文件内容。

SDK snapshot 建立后立即归还 bridge 的 caller 引用；import 还会归还 Memory allocation owner 并 begin_close。此时 SDK 仍保持 bridge 和 READ lease。查询返回 result 后先释放 snapshot，再完整消费每个 method row 的数值及全部 UTF-16 owner/name/prototype，最后释放 result。跨线程用例在 worker 上完成最后 result release，真正触发 Memory lease 归还、origin 最后引用释放及 vector 销毁；主线程 join 后才 collect/drain/destroy 域与销毁回调 context。

线程交接将 move-only `Result` owner 移入线程 callable，不先调用 `result.release()` 交出裸指针。若线程构造抛出异常，callable 的 RAII 析构仍归还 SDK 引用，不能把来源 lease 或 origin 丢失；已成功创建的线程必须 join 后才销毁 bridge/domain。该失败原子性来自所有权结构及源码审阅，本批未通过资源耗尽故意制造操作系统线程创建失败。

活跃 lease 期间 collect 必须为 BUSY、drain(0) 必须 TIMEOUT。超时不强制 free；所有正常用例最终验证 SDK borrowed/jobs 为零、origin 已销毁、回调配对、Memory backing/reservation/allocations/leases/imports 为零且域 CLOSED。违约 poison 不在本消费者中注入。

## 功能与有界稳定性

`tools/verify_dex_import.py` 验证冻结 SDK 的 65 个文件、真实输入的 SHA256/大小，以及每次完整查询的 54,147 方法与 checksum `12701547145945416225`。功能矩阵为普通借用 4 项、导入 8 项：

| 用例 | 验收 |
|---|---|
| normal / cross-thread | 完整输出一致；提前放弃 caller/owner 和 snapshot 后 result 延寿；跨线程最后归还 |
| corrupt / borrow-budget | parser 明确拒绝；拒绝前后各自正确的 callback 计数；输入与域最终归零 |
| import-budget | 全 capacity 超预算在 origin retain 前拒绝；显式恢复预算后再完成完整任务 |
| foreign-cookie | 错 runtime cookie 在 retain 前拒绝；使用当前 cookie 完成完整任务 |
| duplicate | 同 identity/region 的第二 owner 被 BUSY 拒绝且不增加 retain；原任务继续有效 |
| soft-budget | 预算降为 0 后拒绝新 backing，现存 lease 继续完成；worker 最后释放 |

全部 import 用例额外验证 READ 导入拒绝 WRITE lease，snapshot 的 backing 和 import subset 都等于完整 capacity，provider published 为 0。不同虚拟地址的物理 alias 自动识别、任意 runtime 重包装、GPU/ART/ByteBuffer 接管均不在此合同内。

默认回归仅为 direct/import 各 4 次完整 owner task，分别在一个进程中循环创建、查询和关闭；每次都重新读取并最终释放 source。`--cycles` 为最小完成次数（1..65536），`--min-duration-ms` 为最小实际工作时长（0..900000），两项均达到才 PASS，不 sleep 凑时长。`--wall-ms`（1..1200000）限制新轮准入，在途完整任务仍完成；循环 cap 或 wall 先到则记录 INCOMPLETE 并返回 2，外层执行器另设硬 timeout。收据记录 actual_elapsed_ms 与真实完成次数，不将 15 分钟有界运行称为 24 小时稳定性、恒定 RSS 或没有所有类型泄漏。

验证器可用 `--stability-cycles 64 --stability-min-seconds 900 --wall-seconds 1200` 请求两组各至少 64 次且至少 15 分钟的独立 profile。另有实际 1 秒最小工作时长和 1 毫秒新轮准入 wall 的短门槛验证，后者须 INCOMPLETE、不能伪造长稳 PASS。设备 profile 由主执行器统一准入与运行；本模块作者不调用 ADB。超界参数和未知 case 必须明确拒绝。

本模块作者完成的 host 短回归收据为 [host-final-receipt.json](../artifacts/continuation-20261030/dex/host-final-receipt.json)：12 项功能矩阵、两组各 4 次跨线程循环及独立时长/中止门槛。它本身不构成 15 分钟长稳证据。首轮 duplicate 用例因域预算恰好用满先返回 OUT_OF_BUDGET，未触达重复区域检查；初轮原始输出保留，修正仅为该用例显式增加 reservation headroom，使其确实验证 registry 的 BUSY 拒绝，再恢复预算。

线程交接改为捕获 `Result` owner 后另行重建并复跑相同短矩阵，收据为 [host-postreview-receipt.json](../artifacts/continuation-20261030/dex/host-postreview-receipt.json)；前一收据保留其原 source/binary 身份，不升级为新源码的验证。

## 资源与性能归因

两组都没有额外 input backing 或输入复制，source+input 唯一容量均为 source capacity。import snapshot 中的容量是这份 origin 存储的管理视角，**不能与 source capacity 再相加**。普通直接借用本来就能省去复制，这项收益不再计作 Memory 增量。

Memory 导入在此提供集中预算、READ lease、跨关闭寿命和可观测账目；它仍有 metadata/引用计数/域操作成本。parser 内部内存由原 SDK 管理，另记 peak。当前功能/稳定性运行中的 duration 只说明实际覆盖时间，未冻结性能协议，不能据其比较耗时、RSS/PSS、能量或 TLB 净收益。旧 D-C2/D-Z0 的性能记录也不会因新导入功能而被改写。

## Arena 导入与消费者结果工作区

`mgmem_dex_arena_import_consumer` 是通过 CMake `MGMEM_DEX_ENABLE_ARENA_IMPORT=ON` 显式启用的独立编译目标，要求同一安装版运行库提供 `mgmem_import_external_arena_v1` 和 `MGMEM_CAP_ARENA_EXTERNAL_IMPORT`；默认不构建，也不更改原目标的旧库动态链接要求。其 `arena-import` 路径把 DEX 全容量以只读方式导入 Arena 域，并在同一域申请 4096 字节工作区（8192 字节 Arena backing）。结果查询时以工作区批量缓存方法字段，再按原顺序计算同一个完整结果 checksum；SDK parser 的内部内存没有移入该域。

原始输入和 Memory 的外部导入是同一 backing，只有一份容量；域的 backing 另包含独立的 8192 字节 Arena 块。功能核验检查这两部分、批量工作区实际使用、同一查询 oracle、预算拒绝、parser 拒绝、跨线程最终释放和域关闭后归零。作用域是独立消费者功能，不代表零复制以外的额外节约或任何性能收益。

`tools/verify_dex_arena_import.py` 通过显式传入新旧二进制、安装库、锁定 SDK、真实输入和输出路径，校验直接借用基线与六组 Arena 导入功能。它不运行 ADB，输出仅含输入摘要、容量和验证结果，不发布本地路径或输入；旧 `tools/verify_dex_import.py` 及其 direct/import 合同保持独立。

## 复跑

```powershell
tools/host-build.cmd -S consumers/dex -B build/continuation-20261030/dex-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=安装后的0.2目录 -DDEXMEGUVIZ_SOURCE_DIR=冻结SDK目录
tools/host-build.cmd --build build/continuation-20261030/dex-host --target mgmem_dex_import_consumer --parallel 2
python tools/verify_dex_import.py --input 本地真实classes.dex
```

生产源、CMake、验证器、二进制、实际加载候选库及冻结 SDK 逐文件 hash 随收据记录；输入只发布 SHA256 和大小。该工具不下载输入、不调用 ADB、不修改外部仓库。设备执行、API34+、R8、16 KB、MTE 和产品资格须查看各自独立收据，不能从 host 结果推导。
