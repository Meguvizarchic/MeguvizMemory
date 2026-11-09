# Allocation 与 owner 清单

本清单对应CPU工程预览，资源合同见ADR-001。0.2/ABI1.1预算约束自有及导入region的唯一请求容量与在途reservation；容器/allocator元数据、OS驻留和其他库资源另记。导入子集账目不再与backing重复相加，详见[外部导入合同](EXTERNAL_IMPORT.md)。

| 存储 | origin-owner / 最终释放者 | 借用与线程 | 结束条件 |
|---|---|---|---|
| 导入CPU region（ABI1.1） | 外部origin-owner，Memory retain/release其引用，从不自行free | 一个manager runtime整region唯一owner；READ不可取得WRITE；子视图沿用borrow | 最后owner/lease结束且release callback完成后解除claim；违约不确定资源隔离保留 |
| System backing | 创建 domain 时复制的 provider；默认 `_aligned_malloc/_aligned_free` 或 `posix_memalign/free` | 同域 allocation owner，可跨线程取得/归还读写 lease | owner 与最后 lease 都退出；由原 provider 同步释放 |
| arena chunk | 同上，domain 保持唯一 backing 清单 | allocation 在 chunk 内有不重叠 offset/length；所有子对象共享 backing 记账 | 无 owner/lease/pending 时 reset 复用；trim/collect 最终释放 |
| allocation/lease 元数据 | core 的内联记录及私有连续句柄表 | 核心 mutex 下访问；表记录可移动，公开 ID 与 payload 地址不变 | lease 归还即移除其记录；allocation 在最后 owner/lease 退出后移除，或申请失败回滚；空表归还数组，仍有活动记录时可保留较大容量 |
| provider context | 外部 provider；domain retain/release 保活 | 回调锁外并发，provider 必须线程安全且 no-throw | collect 后 destroy；不能超时强制销毁 |
| C++ wrapper state | `shared_ptr<domain_state>` | domain/owned_allocation/borrow 各保活；同一 wrapper 变化须外部同步 | 最后 wrapper 在 cleanup worker 同步关闭；析构清理异常 terminate |
| Dex 输入字节 | Memory allocation | 先 writer 装载，再 immutable reader；原子 callback context 将同一个 Memory lease 延寿给真实 Dex snapshot/result | 原 owner 提前 release；snapshot release 后 result 仍保活；最后 SDK release 归还 lease |
| Dex 解析 facts / 查询结果 | DexMeguviz 引擎 | Memory 不接管其内部容器或 miniz | 原 Dex API 释放；这里只观察 borrowed_bytes，不重复记入 Memory backing |
| 图像 RGB/luma/blur | Memory 的三个 allocation | worker 入队前取得全部 lease；RGB reader、两个 writer；owner 关闭后 worker 继续完成并写 PGM | worker 最后访问后归还 lease；主调用方 join 后 collect/drain/destroy |
| BIL 现有 FrameScratchArena | Java direct buffer 原 owner | 五槽、四输入与单独输出，已有初始化和成功合同 | 原代码清引用不等于即时释放；本批没有迁入 Memory |
| Compute GPU/AHB 与 Media frame | Compute/平台/Media 原 owner | 原 content lease、map/unmap、fence、retirement | 原库确认完成后释放；Memory CPU collect 不能替代 GPU retirement |

live 和 retiring 为互斥 payload，retained=backing-live-retiring，包含 padding 和 arena reset 前不能复用的空间；backing 是父项，不能和子项再次相加。增长先申请新 backing、后释放旧 owner，旧新共存峰值计入预算。allocation_count 计成功发布的逻辑申请；provider_allocation_count 计成功发布的 backing，不包含失败和 close 回滚尝试，完整回调次数须由诊断 provider 记录。

原始地址只在 lease 存续期间有效。runtime_origin 是当前装载模块的路由 cookie；domain/resource ID 在模块内单调不复用。句柄、裸指针和 domain 指针不得在运行库卸载后继续使用。
