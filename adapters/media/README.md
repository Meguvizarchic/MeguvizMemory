# Media CPU READ → Memory 桥

短名入口和完整资源释放示例见 [只读导入调用](../compute/SHORT_NAMES.md)。

本独立适配器将 `MgmFrame` 的完整 CPU READ lease 交给 Compute–Memory 桥进行零复制导入。调用方须持有帧引用直到导入调用返回；适配器在获取底层 lease 期间会独立持有帧引用，导入成功后由 Compute 桥保活底层映射。此后 `MgmFrame` 与 Memory `owner` 可各自独立释放，直到最后一个 Memory 借用归还才解开映射。

System 域导入请链接 `MeguvizMemoryMediaReadBridge::read_bridge`；需要在 Arena 域导入外部只读帧时，链接独立的 `MeguvizMemoryMediaReadBridge::arena_read_bridge` 并满足 Compute Arena 桥的能力要求。源 Compute/Media 与 Memory 运行库、`external_id`、只读别名、完整范围、就绪状态及错误回调规则均继承 [Compute 桥合同](../compute/README.md)；GPU、`AHardwareBuffer` 与受保护内容沿用 Compute 映射规则。

若基于未导出 Arena 目标的旧版 Compute 桥构建，只会生成 System 目标；消费方可通过 CMake `if(TARGET MeguvizMemoryMediaReadBridge::arena_read_bridge)` 或在 `find_package` 中指定 `COMPONENTS arena_read_bridge` 检查能力。

构建本适配器时需分别传入 Memory、Compute、Media 以及 Compute–Memory 桥的安装前缀，Memory 核心与普通消费方不会因此自动引入 Media 依赖：

```sh
cmake -S adapters/media -B build/media-read-bridge \
  -DCMAKE_PREFIX_PATH="<Memory>;<Compute>;<Media>;<ComputeBridge>"
cmake --build build/media-read-bridge --config Release
ctest --test-dir build/media-read-bridge -C Release --output-on-failure
```
