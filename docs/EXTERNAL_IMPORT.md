# 外部内存导入与软预算（ABI 1.1）

ABI `1.1` 新增了 `mgmem_import_external_v1`、`mgmem_domain_import_snapshot_v1` 和 `mgmem_domain_set_budget_v1`（以及后续可选扩展 `mgmem_import_external_checked_v1`、`mgmem_import_external_arena_v1`、`mgmem_import_external_arena_checked_v1`），同时保持 ABI `1.0` 的 POD 布局与旧符号不变。C++ 接口 `domain::import_external` 返回标准的 `owned_allocation`，借用、跨线程延寿和关闭流程沿用既有状态机。本机制不接管 ART、Java `DirectByteBuffer`、AHB 或 GPU 回收，也不支持任意分配器之间互换释放。

## 唯一 backing 合同

`mgmem_external_buffer_v1` 必须描述一块完整、真实且地址稳定的外部存储：包含起始地址、完整可访问容量、对齐、外部 owner 分配的非零稳定 ID、访问权限以及 `retain`/`release` 回调。一次导入创建一个 `owner` 句柄，其**完整容量**计入当前域预算；多个子区间通过同一句柄的 `offset`/`length` 借用表达，无需重复导入或重复计费。原有的两个导入入口仍仅接收 System 域，传入 Arena 域时返回 `MGMEM_UNSUPPORTED`。

可选扩展 `mgmem_import_external_arena_v1` 和 `mgmem_import_external_arena_checked_v1` 分别使用普通和 checked 描述符，且仅接收 Arena 域。调用方须在同一份运行库上先确认 `MGMEM_CAP_ARENA_EXTERNAL_IMPORT` 并解析对应符号（checked 版本还须确认 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE`），不能仅凭 ABI `1.1` 推断扩展存在。旧版运行库不包含这些符号，本扩展也不会改变任何公开结构体布局或旧入口语义。

在 Arena 域中，外部 backing 与自有 `chunk` 共用同一个软预算，但绝不作为 Arena 子分配或 `reset` 的可复用空间；最后一个 `owner` 或 `lease` 归还后，会在锁外立即调用来源释放函数，而不等待 `reset`、`trim` 或域关闭。域处于 `OPEN` 且未隔离时，只要存在未归还的 `owner`、`lease` 或释放回调，`reset` 和 `trim` 均返回 `MGMEM_BUSY`。回调完成后，`reset` 只重置自有 `chunk`，`trim` 在域空闲时归还自有 backing；若 checked release 失败，域仍保留未确认记录与计费并进入隔离，绝不重试该回调。

`external_id` 在同一运行库的所有活跃导入中必须唯一对应这块区域及其来源 owner，不同来源之间也不得复用相同 ID。禁止为同一块内存的子区间伪造新 ID 来规避预算检查。模块级注册表会拒绝重复 ID，以及与任何已登记自有或外部区域重叠的区间（包含跨域和正在进行中的导入），相邻不重叠的区间则允许。重叠检查以虚拟地址范围为依据，无法识别映射到同一物理页的不同虚拟地址。

描述符的 `managing_runtime_origin` 字段必须显式设为管理它的 `mgmem_runtime_origin_v1()`；将该结构体传给另一份已装载的运行库会被路由校验拒绝。C 与 C++ 包装均会拒绝 cookie 为 `0` 或来源不匹配的描述符，不作自动改写，避免同一描述符在多个运行库中被重复接收。该 cookie 仅用于防止误路由，不代替物理内存归属校验；同一物理区域必须只交给一个管理运行库。

调用方须提供有效内存与真实来源 owner，保证自身引用存活至导入调用返回，并在整个 `retained` 期间维持地址与容量稳定。来源回调代码及其所属动态库也须保持加载（`retain` 不会自动增加动态库加载引用）。`MGMEM_READ` 禁止获取写借用，但不修改页保护位；C 调用方不得通过裸指针越权写入，外部别名也须遵守来源方的同步协议。

## 生命周期、失败和关闭

导入先在锁内完成参数校验、预算预留和元数据登记，随后在脱离域锁与注册表锁后调用 `retain`。`retain` 返回非 `MGMEM_OK` 时必须保证失败原子性（不增加引用计数），此时不会触发 `release`。若 `retain` 成功后恰好并发关闭，会在锁外调用 `release` 回滚并将输出句柄置空；地址、对齐、`size_t` 范围、ID、版本校验或预算不足均会在调用 `retain` 之前直接拒绝。

归还 `owner` 后，已取得的 `lease` 仍会继续保活外部存储，直到最后一个 `lease` 释放时才触发来源 `release`；Memory 绝不对外部地址调用 `free` 或域自身的 `provider->deallocate`。由于 `release` 可能同步阻塞，应安排在允许清理的线程执行。回调执行期间，进行中的预留、容量计费和注册表登记均保持有效，确保后续新申请或重叠导入无法绕过预算。`begin_close` 不会撤销已授予的 `lease`，`collect`/`drain`/`destroy` 也不会强制回收它们。

锁序固定为 `domain` → `registry`，或在不持有 `domain` 时单独获取 `registry`，严禁反向加锁；所有 provider 与外部回调均在两类锁之外运行。若 provider 正常复用了刚释放的地址，只有当旧记录已处于无 `owner`/`lease` 的回调清理阶段时，才允许新自有分配暂时重叠；外部导入则必须等待旧记录彻底解除。若 provider 返回与活跃自有或外部区域冲突的地址，属于违反接口约定，核心会隔离该请求而不调用可能释放原对象的释放函数。

如果 `retain` 或 `release` 违约抛出异常，由于引用计数或释放状态未知，核心会将域置为隔离态（poisoned）、保留未确认容量和登记记录，既不重试也不强行释放。正常使用必须严格遵守线程安全、不抛异常及失败原子的回调规则。

## 可报告失败的外部释放扩展

`mgmem_import_external_checked_v1` 接收 `mgmem_external_buffer_checked_v1`，沿用完整的区域、运行库、预算与借用规则，区别在于来源方提供带返回值的 `release_checked` 回调。只有返回 `MGMEM_OK` 才代表外部释放成功；任何其他返回值表示所有权状态未知，核心将返回 `MGMEM_PROVIDER_ERROR`、保留该区域登记与计费、隔离域并停止重试该回调，也不会将域标记为正常关闭。此时最后一个 `owner` 或 `lease` 句柄已被消费，禁止重试同一句柄；若 `retain` 成功后遇到并发关闭回滚，也遵守相同规则。

原有 `mgmem_import_external_v1` 与 ABI `1.0`/`1.1` 数据结构及复用行为不变。本扩展为可选项，**不能仅凭 ABI minor 为 `1` 就认为可用**：须在选定运行库上解析新符号并核对 `MGMEM_CAP_CHECKED_EXTERNAL_RELEASE`。由于 `release_checked` 执行期间区域登记仍保持占用以防失败时提前放行，如果来源方在回调返回前就提前释放了地址，恰好复用该地址的普通 provider 分配会被保守判定为地址冲突并触发隔离，因此来源方应避免在回调尚未返回时复用该地址。

## 软预算与观测

`mgmem_domain_set_budget_v1` 可在 `OPEN` 域上调整容量上限，允许设为 `0` 或低于当前已占容量加上预留。既有资源以及已拿到预留的申请或导入会继续完成；在当前计费超过新上限期间，后续新申请与导入均被拒绝（即使 Arena 内部存在空闲保留空间也无法绕过）。恢复容量上限须由调用方显式重新设回，不会隐式关闭、移动或释放既有资源。

原快照的 `backing_bytes`、`reserved_bytes` 与 `peak_backing_bytes` 现已包含外部导入容量。新增的 `mgmem_import_snapshot_v1` 仅作为其中的子集视图（汇报外部已提交字节与数量、进行中的导入字节、累计导入次数以及 `poisoned` 标记），**不可再与旧快照总数相加**。`provider_allocation_count` 仍只统计自有 provider 申请，外部导入不增加该计数。所有容量统计均指申报逻辑容量，而非 `malloc_usable_size`、元数据、RSS/PSS 或物理内存。

当消费方导入一个预读的 `vector` 时，外部 `vector` 容量与 Memory 的导入 `backing` 只是同一块物理区域的两个记账视角，端到端统计时应按区域身份去重，切勿重复相加；只有先分配再拷贝数据的旧路径才需要把两份独立存储相加。

## 适用范围与验证说明

`tests/import_tests.cpp` 的 7 组常规测试覆盖参数/溢出/预算前拒绝、`retain` 失败原子性、重复 ID/重叠/跨 `domain` 拒绝、自有存储别名、只读与全范围互斥、关闭竞争、进行中导入/provider 软降预算、回调期间计费保持、跨线程最终释放以及 C++ RAII 延寿，正常路径全部归零销毁。3 个独立的 `--poison-*` 用例通过注入回调违约或别名冲突验证隔离保留逻辑后以 `_Exit` 结束，不代表正常回收。ASan 与 Release 回归分别执行，宿主测试通过不代表 Android 加载器、16 KiB 页、MTE 或设备性能资格。
