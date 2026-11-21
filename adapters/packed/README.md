# Packed reader

本模块是独立、无第三方生产依赖的 Java 11 适配器。生产 JAR 仅包含 `io.github.jichuo1.meguviz.memory.packed.PackedDanmakuReader` 及其嵌套类型，不含 BIL 源码、测试类或 Kotlin 运行时。

它直接读取已有的 `int[]` `flat` 与 `byte[]` `reply`，按需提供原始类型字段和保留/改色位图，不构造逐条 `ElemSignal` 对象或 `List<Integer>`。这属于普通消费方的对象实例化优化，与 Memory C ABI 没有依赖关系，也不能计作 Memory 净收益。

使用前请先阅读 [完整合同与证据](../../docs/PACKED_READER.md)。`copyOf` 默认拷贝隔离输入别名；`borrowImmutable` / `selectImmutable` 要求调用方在 `reader` 关闭前保持 `flat` 和 `reply` 不变。`close()` 仅清空内部引用，不保证立即触发 GC 或进程 RSS 下降。

```java
try (PackedDanmakuReader reader = PackedDanmakuReader.copyOf(flat, replyBytes, visibleBudget)) {
    PackedDanmakuReader.Decision decision = reader.decide(
        hasThreshold, minimumWeight, removeVipColorful, vipValue, semanticBlocked);
    // 由原调用方保留 !changed、原 wire rewrite、rescan 与 host parseFrom 步骤。
    // decision 的三份独立 boolean 数组可传给既有 rewriter，不改变原生引擎的启用条件。
}
```

## 适用范围与验证说明

在仓库根目录使用本机 JDK 离线验证与打包：

```text
python tools/verify_packed_reader.py --java-only --build-dir build/packed-java-ci --receipt build/packed-java-ci/receipt.json
python tools/verify_packed_reader.py --bil-source <只读 BIL 目录> --kotlin-lib <已有 Kotlin compiler jar 目录>
```

第二条命令会核验并复制两份真实 oracle 源码到本仓库 `build/` 目录进行独立编译，不会运行 Gradle 或修改/构建外部 BIL 仓库。产物 JAR 默认输出到 `build/packed-reader/`，不是已发布制品或 BIL 生产接入产物。
