# Android 快速接入

本轮通用 AAR 把同一候选的 arm64-v8a、armeabi-v7a、x86_64 放在一个文件中；Java 类、keep 规则、manifest 和 notice 三份逐字节相同，六个共享库逐项继承自对应单 ABI AAR，并与该 ABI SDK 一致。原候选 ZIP 和三个 SDK 保留。

## Java / Kotlin 应用

把 `MeguvizMemory-bfd0e65-universal.aar` 放入应用的 `libs/`，沿用项目现有 Android Gradle 工具链。最低支持 API 27，已有更高的 minSdk 保持原值：

```kotlin
// app/build.gradle.kts
android { defaultConfig { minSdk = 27 } }
dependencies {
    implementation(files("libs/MeguvizMemory-bfd0e65-universal.aar"))
}
```

Java 托管域与 lease 接口由 AAR 提供。`input`、`output` 是调用方已持有的 `ManagedMemoryDomain.Allocation`，以下代码在工作线程执行，`count` 最大为1 MiB：

```java
try (ManagedMemoryDomain.Lease source = input.borrowRead(0, count);
     ManagedMemoryDomain.Lease target = output.borrowWrite(0, count)) {
    source.copyTo(target, 0, 0, count);
}
```

两端已有 lease 时，原生层按 memmove 语义复制，不经过 Java 数组中转。先取得 lease 再提交异步任务，任务结束后关闭它；调用方继续负责 owner 和域的 `beginClose`、完成确认与 `forgetClosed`。关闭超时不强制释放。完整[托管生命周期](ANDROID_MANAGED_DOMAIN.md)及[借用合同](ANDROID_NATIVE_BORROW.md)保持适用。

新 Java 与原生库须使用同一批制品。不要把这个 AAR 的类与旧 JNI 混配；旧库可能没有 nativeTransfer。应用已有的 ABI 选择可缩小最终 APK，不必手工拆分或拼装此 AAR。

## 仅 C / C++ 的 Android 消费者

从同一候选解开对应 ABI 的 `MeguvizMemory-sdk.zip`，分别放在 `third_party/mgmem/<ABI>/`。在 Android NDK 配置中显式设置包目录：

```cmake
set(MeguvizMemory_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/mgmem/${ANDROID_ABI}/lib/cmake/MeguvizMemory")
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(app_native PRIVATE MeguvizMemory::mgmem)
```

此示例使用 SDK 自身提供的共享核心。需要原生注册 JNI 借用/压力时，要求 `COMPONENTS android_jni` 并链接 `MeguvizMemory::android_jni`，详见[候选SDK合同](ANDROID_CANDIDATE_BUILDER.md)。Java AAR 与 native SDK 联合接入时必须同批、最终只装载同一个共享核心；不要另把相同库手工复制进 jniLibs，也不要用 pickFirst 掩盖不同运行库的冲突。

## 重建通用 AAR 与证据边界

```text
python tools/merge_android_candidate_aar.py --candidate dist/MeguvizMemory-Android-Candidate-bfd0e65-20261109.zip --candidate-sha256 7fe27be15cf7a12469959a3283c3fb9cd7e2aa2d78941b570ec0a08e48b451bd --output dist/MeguvizMemory-bfd0e65-universal.aar
```

输出必须是新的 dist AAR，工具拒绝覆盖。它核对输入候选摘要、共同成员、两库/ABI 闭包及 SDK/AAR 身份，并在发布前逐成员回读；没有重新编译 Java 或原生代码。

单 ABI 候选、R8 诊断和 arm64 运行记录不能自动升级成“由通用 AAR 构建的应用已验证”。本次通用制品只取得成员字节继承与归档回读证据；仍应以应用实际构建及安装结果确认最终 APK 的 ABI、签名和原生库包装。16 KiB loader、MTE、产品性能与正式发布资格分别查看[工程状态](../STATE.md)。
