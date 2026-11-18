# Media CPU READ → Memory 桥

短名入口和完整资源释放示例见 [只读导入调用](../compute/SHORT_NAMES.md)。

独立适配器将 Media 帧的完整 CPU READ lease 交给 Compute–Memory 桥，不复制像素。调用方须在导入函数返回前持有帧引用；适配器在取得底层 lease 期间持有自己的帧引用，导入成功后由 Compute 桥保活原映射。帧和 Memory owner 可独立释放，最后一个 Memory lease 决定映射归还时机。

使用 `MeguvizMemoryMediaReadBridge::read_bridge` 导入 SYSTEM 域。需要 Arena 外部导入时选择独立的 `MeguvizMemoryMediaReadBridge::arena_read_bridge`，并满足原 Compute Arena 桥的能力要求。源 Compute/Media 和 Memory runtime、外部 ID、只读别名、完整范围、就绪状态及错误回调要求继承 [Compute 桥合同](../compute/README.md)。GPU、AHardwareBuffer 和受保护内容沿原 Compute 映射合同处理。

使用未导出 Arena 桥目标的旧版 Compute 桥构建时，只生成 SYSTEM 目标。调用方可以通过 CMake 的 `if(TARGET MeguvizMemoryMediaReadBridge::arena_read_bridge)` 检查 Arena 能力，或在 `find_package` 中要求 `COMPONENTS arena_read_bridge`。

构建时分别提供 Memory、Compute、Media 和 Compute–Memory 桥的安装前缀。桥为独立 CMake 项目，Memory 核心和现有消费者不自动增加 Media 依赖。

```sh
cmake -S adapters/media -B build/media-read-bridge \
  -DCMAKE_PREFIX_PATH="<Memory>;<Compute>;<Media>;<ComputeBridge>"
cmake --build build/media-read-bridge --config Release
ctest --test-dir build/media-read-bridge -C Release --output-on-failure
```
