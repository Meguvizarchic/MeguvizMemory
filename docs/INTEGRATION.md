# 安装、消费、关闭与回退

当前功能预览为1.2.1、native ABI1.1，旧ABI1.0的POD布局和符号保持兼容。除原CPU backing、owner/lease、System/arena外，新增唯一外部region导入、软预算及独立Android压力适配。新安装前缀为 `install/features-host` / `install/features-arm64`，不覆盖历史0.1 SDK；旧实验和下文原双消费者收据仍适用于其固定快照。新功能用法与限定验证范围见[阶段收尾](FEATURE_STAGE_20261030.md)、[导入](EXTERNAL_IMPORT.md)、[Android压力](ANDROID_PRESSURE.md)和[packed reader](PACKED_READER.md)。功能、设备、家族集成、性能和发布继续分别资格。

## 安装后的独立消费

在已配置 CMake 和 C++ 编译器的终端执行以下重建步骤。Windows 可以使用 `tools/host-build.cmd` 进入已配置的 MSVC 环境并转发 CMake 参数；该脚本不自动建立消费者运行时 PATH。

```powershell
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --config Release
ctest --test-dir build/host -C Release --output-on-failure
cmake --install build/host --config Release --prefix "$PWD/install/host"

cmake -S consumers/image -B build/image -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$PWD/install/host"
cmake --build build/image --config Release

cmake -S consumers/dex -B build/dex -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$PWD/install/host" "-DDEXMEGUVIZ_SOURCE_DIR=$PWD/build/locked-inputs/DexMeguviz"
cmake --build build/dex --config Release --target mgmem_dex_consumer
```

以上是复现入口，不表示只阅读本文便完成了运行。生成器和工具链必须与所用二进制目录一致；不要在同一个目录更换 Ninja/Visual Studio 或编译器。多配置生成器将消费者可执行文件置于配置子目录。Windows 运行前让 `install/host/bin` 位于本次终端的 PATH，Linux 使用安装后对应共享库的合法搜索路径。需要记录实际加载的库路径与哈希，不能因能找到另一个旧 DLL 而算安装消费通过。

独立工程使用：

```cmake
find_package(MeguvizMemory 0.1 CONFIG REQUIRED)
target_link_libraries(my_cpu_consumer PRIVATE MeguvizMemory::mgmem)
```

两个现有入口分别是 `mgmem_image input.ppm output.pgm [--system]` 和 `mgmem_dex_consumer input.dex [--arena]`。图像程序读取 P6 RGB、转换灰度并做模糊；Dex 程序读取真实 DEX 字节、调用真实 DexMeguviz borrowed-source/查询 API，并检查结果跨 snapshot 的寿命。前者默认 arena，后者默认 System。它们从安装包导入 Memory；Dex 自身采用显式指定的只读源码输入，不将其说成安装后的 Dex SDK。

Dex 工程没有有效 Git HEAD，存在其他会话的开发材料。使用它时锁定实际参与编译源码与 fixture 的哈希，并在构建前后检查漂移。它的已有 miniz 是解析器内部依赖，Memory core 不引入该依赖。文件格式 fixture 的功能通过不等于 BIL 宿主或真实 APK 场景资格。

收尾核验期间，外部会话更新了 Dex 的 CMake 和任务元数据。因此最终验证改用本工作区 `build/locked-inputs/DexMeguviz` 的冻结快照，以 `build/dex-locked` 构建，fixture 也从该快照读取。最终 SOURCE_LOCK 的 DexMeguviz 根指向此快照，不再要求活跃外库永久保持不变。可重放输入另存为本地 `dist/MeguvizMemory-0.1.0-DexConsumer-inputs.zip`，仅记录 hash，不向公开 Git 仓库复制整个外部项目。复现时优先解压该输入并把 `DEXMEGUVIZ_SOURCE_DIR` 指向它。

## 申请、借用与原始 owner

创建 domain 时固定 provider、模式和 backing 预算。配置表携带 `struct_size`、ABI major/minor 和必需 flag；不支持的版本或必需能力明确拒绝。System 通过每次逻辑申请建立独立 backing；arena 在自有 chunk 中密集分配，最后一次使用结束后才允许 reset 复用。

`mgmem_allocate_v1` 返回 allocation owner。写入先取得 `MGMEM_WRITE` lease，完成后释放；只读消费先取得 `MGMEM_READ` lease。读 lease 可共存，写 lease 排斥同 allocation 上的其他 lease。异步任务必须在接受/入队前取得 lease；失败、取消和回调异常路径也必须归还。释放 owner 后禁止新 borrow，已有 lease 继续保活 backing，直到最后一次访问完成。

借用地址不交给 `free`、`delete`、别库的释放函数或另一个 provider。allocation、lease 与 domain 必须始终由创建该 domain 的同一套 runtime API 管理；`runtime_origin` 负责拒绝错误来源的句柄，不能替调用方推断或调度正确的原始函数表。原始 domain 指针在 destroy 后失效，所有 destroy 与其他 API 调用由消费者串行化。

容量增长使用“申请新 backing → 保持必要旧访问 → 迁移/初始化 → 发布新 owner → 旧 lease 完成后释放”的显式过程。预算涵盖旧新 backing 共存和 pending reservation。OOM/预算拒绝不能丢弃仍有用的旧 owner，也不能偷偷减少工作量。Memory 默认不清零，输入初始化是消费者责任，必须计入与基线一致的工作量和阶段。

快照的 `backing_bytes` 是唯一managed请求容量，0.2起同时包含owned与imported backing。live/retiring/retained和新import snapshot均为相应子项，不能再次与backing相加。预算不包括allocator/核心元数据、RSS/PSS或外部GPU/驱动容量。其他库观察同一backing时记录借用关系，不能重复计为第二份CPU所有权。

## Provider 合同

核心复制 vtable 并 retain context；最终成功关闭且销毁 domain 后 release context。所有 allocate/deallocate/retain/release 回调都必须线程安全，包括不同 domain 共用 context 的场景；回调不能抛异常，也不能重入同一 domain。调用发生在核心锁外，不代表回调被串行化。

allocate 必须失败原子：成功返回指定容量和对齐的未初始化存储；失败留下空输出。deallocate 必须使用最初的地址、容量、对齐和原始 provider。返回成功后仍未分配、失败时偷偷留下资源、抛异常后是否释放不明，都是 provider 违约，无法通过通用重试安全推断。

回收回调违约使 domain poisoned，核心保留错误及未确认释放的记账，不自动重试可能已经释放的地址。最终 destroy 调用 provider.release 后若回调抛异常，domain 已被消耗，返回 `MGMEM_PROVIDER_ERROR` 也不能重试或访问该指针。`MGMEM_BUSY` 的前置条件拒绝则不消耗 domain。正常 provider 必须遵守 no-throw 合同，使该违约分支不会发生。

## 关闭与清理线程

状态为 `OPEN → CLOSING → CLOSED`。`mgmem_domain_begin_close_v1` 关闭新申请/新 borrow 的准入，不等待任务、lease 或 provider 回收。它是普通同步 API，内部短期互斥不构成硬实时或 lock-free 保证；UI 线程不得执行可能等待自身任务的清理。

正常顺序：

1. 停止接收本域新任务并调用 `begin_close`。
2. 已受理任务继续完成或取消，按原寿命释放 allocation owner 与 lease。
3. 清理 worker 调用 `mgmem_domain_collect_v1`。存在 owner、lease、申请或另一轮回收时返回 `MGMEM_BUSY`；条件具备时同步调用原 provider 回收，成功后进入 CLOSED。该调用可能受 provider 时延影响，不能放在负责完成任务的 UI/worker 上等待。
4. 观察方用 `mgmem_domain_drain_v1` 等待 CLOSED；`timeout_ms=0` 仅轮询，drain 从不执行 provider 回调，也不代替 collect。超时保留 owner/provider，不释放仍活跃的存储。
5. collect worker 结束、其他 API 不再访问本域后，最终 owner 调用 destroy。

不能只调用 begin_close 和 drain 而忘记安排 collect。不要在 drain 超时后销毁 domain、卸载 DSO、reset、purge 或释放借用 context。`trim` 和 `arena_reset` 同样要求没有活跃 owner/lease/pending 操作；trim 是同步回收，reset 只重置安全闲置的 arena。

## JNI 与家族组合边界

0.2已交付Android压力适配JNI、opaque注册和不逃逸的受控字节lease，支持有边界的 `byte[]` 读写复制；自有托管域也提供此能力，见[压力合同](ANDROID_PRESSURE.md)和[借用合同](ANDROID_NATIVE_BORROW.md)。通用可逃逸Java ByteBuffer借用和Rust bridge仍未选择，也没有让Java任意持有native DirectByteBuffer的接口。generation检查不能使已逃逸的Java ByteBuffer在底层释放后安全。Java/JNI异步任务必须在受理前取得真实保活关系，并逐条验证取消、结果逃逸和关闭。

Compute/Media的真实源码和安装SDK已在相邻MeguvizPack工作区找到，详见[消费者审计](audit/consumers.md)及[当前合同核实](FAMILY_MEMORY_CONTRACTS.md)。Compute管理资源、content lease、submit和GPU retirement；Media管理帧、时间线和背压。可选的 [Compute CPU READ-map → Memory 导入桥](../adapters/compute/README.md)已有宿主限定验证；Memory-owned backing 注入 Compute 的 provider 接口及 Android/产品运行资格仍未具备。CPU内存可回收状态不能用GPU已提交、逻辑帧释放或仅持有AHB对象代替。

官方 Compute/Media/RIFE 组合有最终单 DSO、单 Compute/ncnn/STL 闭包合同。未来链接 Memory 静态 PIC core 时，必须重新核对最终链接图、导出表、依赖闭包、实际加载模块和 origin-owner：一个最终组合只拥有一份 Memory core，不附带第二套 GPU pool/ncnn/runtime。独立 Memory JNI DSO 不能与该组合重复实现后混用句柄。双独立 runtime 拒绝交叉 handle 的宿主测试是防误用证据，不能替代这项最终产物资格。

`runtime_origin` 是当前已装载模块中的路由 cookie，取自该 runtime anchor 的地址。它不是永久资源身份、持久化格式或卸载/重新加载代次。句柄不可跨进程保存恢复；所有 domain、owner、lease、任务和回调必须在 DSO 卸载前销毁，禁止卸载后复用旧句柄。即使重新加载恰好得到同一模块地址，旧指针和句柄仍无效。

## 禁用与回退

默认选择System/dense。arena是显式生命周期策略；placement、page hint、slab、通用ByteBuffer借用和GPU桥均未选择，Android压力JNI独立可用。新设备、固件变化、预算不合格或策略回归时，之后的新domain使用相同适用预算的System provider，必要时由消费者回退现有合格实现并记录原因。

已经创建的 domain 继续使用其原始 provider 完成并清理，不能把旧指针交给新 provider、在线搬移稳定地址、取消寿命检查或通过无界申请掩盖预算拒绝。旧 domain 尚未 collect/destroy 时同时建立替代 domain，需要把两者共存峰值纳入消费者总预算。当前没有经资格验证的特殊 TLB 策略，不能依据芯片营销名称启用它。

发布前必须分别核验 ABI/功能、资源稳定、消费者正确性、设备 loader、产品延迟与性能回归预算。未经采样的收益、能量、PMU/PFN 机制、Android 16KB 真机及 BIL LSPosed 行为保持未验证；单个 fixture 或源码构建通过不会提升这些状态。
