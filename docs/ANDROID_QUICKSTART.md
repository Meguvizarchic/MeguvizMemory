# 在 Android 应用中使用 MeguvizMemory

Java 和 Kotlin 可以直接使用 `ManagedMemoryDomain`。如果应用还要从 C 或 C++ 访问同一块内存，可以使用 AAR 内的 Prefab 模块，也可以按 ABI 链接原生 SDK。

**系统与工具链要求。** `mgmem-android:1.2.1` 的 Gradle AAR 最低可在 API 27 运行，应用编译时需要 Android SDK 37.2 或更高版本。使用 37.2 时，在应用的 `android` 块中写 `compileSdk { version = release(37) { minorApiLevel = 2 } }`。AAR 包含 `arm64-v8a`、`armeabi-v7a` 和 `x86_64`，应用可以只打包自己支持的 ABI。

## 给应用添加依赖

将 AAR 放在应用模块的 `libs/` 目录，再在 `app/build.gradle.kts` 中引用。下面用的是本地 Maven 仓库中的文件名。若拿到的是直接组装的 `mgmem-android-release.aar`，把路径末尾换成该文件名。

```kotlin
dependencies {
    implementation(files("libs/mgmem-android-1.2.1.aar"))
}
```

团队使用 Maven 仓库分发 AAR 时，在项目已有的仓库配置中加入存放该包的仓库，依赖写成 `implementation("io.github.jichuo1.meguviz:mgmem-android:1.2.1")`。版本单独指定。仓库自带的 Gradle 工程可以生成 AAR 和本地 Maven 仓库。[从候选包组装 AAR](../bindings/android/README.md)

## 读写第一块缓冲区

下面的方法接收一个已打开的域，申请一个 4 字节整数的空间，写入后再读出。把方法放进自己的 Java 类，导入 `io.github.jichuo1.meguviz.memory.android.ManagedMemoryDomain` 和 `java.nio.ByteOrder`，在工作线程调用。

```java
static int roundTrip(ManagedMemoryDomain domain, int value) throws Exception {
    try (ManagedMemoryDomain.Allocation slot = domain.allocate(4, 4)) {
        try (ManagedMemoryDomain.Lease writer = slot.borrowWrite(0, 4)) {
            writer.withDirectBuffer(0, 4, b -> b.order(ByteOrder.nativeOrder()).putInt(value));
        }
        try (ManagedMemoryDomain.Lease reader = slot.borrowRead(0, 4)) {
            byte[] result = new byte[4];
            reader.read(0, result, 0, result.length);
            return java.nio.ByteBuffer.wrap(result).order(ByteOrder.nativeOrder()).getInt();
        }
    }
}
```

应用可用 `ManagedMemoryDomain.system(1024 * 1024)` 创建预算为 1 MiB 的域，在多次调用间复用。上例中写借用先归还，读借用才能取得。直接视图及其切片只能在回调内使用。需要在归还 lease 后继续保留数据时，可以像上例那样读到自己的数组中。两个已有 lease 之间的传输用 `copyTo`，单次最多 1 MiB。[借用方式与限制](ANDROID_NATIVE_BORROW.md)

组件退出时，停止提交新任务，调用 `domain.beginClose()`，让在途任务归还 allocation 和 lease。在后台观察到 `CLOSED` 后调用 `domain.forgetClosed()`。调用 `close()` 和 `beginClose()` 一样，都只启动关闭流程。[处理等待和关闭错误](ANDROID_MANAGED_DOMAIN.md#关闭内存域) · [带系统压力回调的后台示例](../consumers/android_managed/README.md)

## 应用含 C 或 C++ 代码

**使用带 Prefab 的 Gradle AAR。** 在应用模块启用 Prefab，然后在应用的 `CMakeLists.txt` 中链接目标。`app_native` 表示应用自己的原生目标。

```kotlin
android {
    buildFeatures { prefab = true }
}
```

```cmake
find_package(mgmem-android CONFIG REQUIRED)
target_link_libraries(app_native PRIVATE mgmem-android::mgmem)
```

需要从原生侧注册借用或压力控制器时，再链接 `mgmem-android::mgmem_pressure_jni`。如果 AAR 内没有 `prefab/prefab.json`，可用同批次的原生 SDK。

**使用独立 SDK。** 将各 ABI 的 SDK 解到 `third_party/mgmem/<ABI>/`，示例路径相对于应用的 `CMakeLists.txt`。

```cmake
set(MeguvizMemory_DIR
    "${CMAKE_CURRENT_LIST_DIR}/third_party/mgmem/${ANDROID_ABI}/lib/cmake/MeguvizMemory")
find_package(MeguvizMemory CONFIG REQUIRED)
target_link_libraries(app_native PRIVATE MeguvizMemory::mgmem)
```

若需 JNI 注册，查找时加 `COMPONENTS android_jni` 并链接 `MeguvizMemory::android_jni`。Java 类、JNI 和核心共享库应来自同一套制品。若 APK 合并时出现重复的 `libmgmem.so`，先检查 Prefab、SDK 和 `jniLibs` 安装了哪几份，确保所有调用方共享同一份核心。[SDK 目录与组件](ANDROID_CANDIDATE_BUILDER.md)
