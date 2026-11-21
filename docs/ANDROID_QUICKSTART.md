# 在 Android 应用中使用 MeguvizMemory

Java 和 Kotlin 可直接使用 `ManagedMemoryDomain`（版本 `1.2.2`）。若应用还需在 C 或 C++ 中访问同一块内存，可启用 AAR 内置的 Prefab 模块，或按 ABI 链接独立的原生 SDK。

**系统与工具链要求**：`mgmem-android:1.2.2` 的 Gradle AAR 支持 `minSdk 27`，编译期需 Android SDK `37.2`（`compileSdk { version = release(37) { minorApiLevel = 2 } }`）与 JDK `17`。AAR 包含 `arm64-v8a`、`armeabi-v7a` 和 `x86_64`，应用可按需打包目标 ABI。

## 给应用添加依赖

**家族 BOM 边界**：`io.github.jichuo1.meguviz:meguviz-bom:1.2.0` 管理家族其余 33 个 `1.2.0` 制品，**不包含** `mgmem-android:1.2.2`。请通过本地 Maven 仓库或直接引入组装好的 AAR 来依赖本库。

将 AAR 放到应用模块的 `libs/` 目录后，在 `app/build.gradle.kts` 中添加依赖（若使用直接组装产物，将文件名改为 `mgmem-android-release.aar`）：

```kotlin
dependencies {
    implementation(files("libs/mgmem-android-1.2.2.aar"))
}
```

若团队通过本地或私有 Maven 仓库分发 AAR，先在工程仓库配置中加入该仓库地址，再引用坐标：

```kotlin
dependencies {
    implementation("io.github.jichuo1.meguviz:mgmem-android:1.2.2")
}
```

从源码组装 AAR 及发布本地 Maven 仓库的步骤见 [Android 打包说明](../bindings/android/README.md)。

## 读写第一块缓冲区

以下方法接收一个已打开的托管域，申请 4 字节整数空间，写入后读回验证。将其放入 Java 类并导入 `io.github.jichuo1.meguviz.memory.android.ManagedMemoryDomain` 与 `java.nio.ByteOrder`，然后在后台工作线程调用：

```java
static int roundTrip(ManagedMemoryDomain domain, int value) throws Exception {
    try (ManagedMemoryDomain.Allocation slot = domain.allocate(4, 4)) {
        try (ManagedMemoryDomain.Lease writer = slot.borrowWrite()) {
            writer.withDirectBuffer(b -> b.order(ByteOrder.nativeOrder()).putInt(value));
        }
        try (ManagedMemoryDomain.Lease reader = slot.borrowRead()) {
            byte[] result = new byte[4];
            reader.read(0, result, 0, result.length);
            return java.nio.ByteBuffer.wrap(result).order(ByteOrder.nativeOrder()).getInt();
        }
    }
}
```

可用 `ManagedMemoryDomain.system(1024 * 1024)` 创建预算为 1 MiB 的域并在多次调用间复用。上例中写借用先关闭归还，随后的读借用才能成功获取；直接视图及其切片仅在 `withDirectBuffer` 回调期间有效。若要在归还借用后继续保留数据，需像上例一样复制到 Java 数组或另一段区域。两个已有借用之间的传输使用 `copyTo`，单次上限为 1 MiB；需要分段读写时可使用带偏移与长度参数的重载（如 `borrowWrite(offset, bytes)` 与 `withDirectBuffer(offset, length, action)`）。[借用方式与限制](ANDROID_NATIVE_BORROW.md)

组件退出时，先停止提交新任务并调用 `domain.beginClose()`（`close()` 与 `beginClose()` 语义相同，均只发起非阻塞关闭请求），待未完成的任务归还全部 `Allocation` 和 `Lease` 后，在后台线程观察到 `CLOSED` 再调用 `domain.forgetClosed()`。[处理等待和关闭错误](ANDROID_MANAGED_DOMAIN.md#关闭内存域) · [带系统压力回调的后台示例](../consumers/android_managed/README.md)

## 应用含 C 或 C++ 代码

**方式一：使用 AAR 内置 Prefab**。在应用模块开启 Prefab 后，直接在应用 `CMakeLists.txt` 中链接对应目标（`app_native` 为应用自身的原生目标）：

```kotlin
android {
    buildFeatures { prefab = true }
}
```

```cmake
find_package(mgmem-android CONFIG REQUIRED)
target_link_libraries(app_native PRIVATE mgmem-android::mgmem)
```

若需从 C/C++ 原生侧向 Java 注册受控借用或压力控制器，再链接 `mgmem-android::mgmem_pressure_jni`。

**方式二：使用独立原生 SDK**。将各 ABI 的独立 SDK 解压至 `third_party/mgmem/<ABI>/`（路径相对于应用的 `CMakeLists.txt`）：

```cmake
set(MeguvizMemory_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/mgmem/${ANDROID_ABI}/lib/cmake/MeguvizMemory")
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(app_native PRIVATE MeguvizMemory::mgmem)
```

如需原生 JNI 注册能力，查找时附加 `COMPONENTS android_jni` 并链接 `MeguvizMemory::android_jni`。Java 类、JNI 库和核心共享库必须来自同一套构建制品；若打包 APK 时提示存在重复的 `libmgmem.so`，请检查 Prefab、独立 SDK 与 `jniLibs` 目录，确保全进程只打包并加载同一份核心共享库。[SDK 目录与组件](ANDROID_CANDIDATE_BUILDER.md)
