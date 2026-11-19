# 构建 Android AAR 和本地 Maven 仓库

此 Gradle 工程将候选 ZIP 中的原生库、仓库中的 Java 源码和公共头文件组装为 `mgmem-android`。成品可供应用直接引用，也可通过本地或团队 Maven 仓库分发。已经拿到 AAR 的应用开发者可以从[Android 快速接入](../../docs/ANDROID_QUICKSTART.md)开始。

## 准备输入

使用 JDK 17、Android SDK 37.2、Build Tools 37.0.0 和本目录的 Gradle Wrapper。将 SDK 路径配置在本机 `local.properties` 或 Android 构建环境中。原生库来自候选 ZIP，Java 和 Prefab 头文件来自当前检出。请选取**同一次源码构建**的输入，否则类可能引用 JNI 中没有的方法。[从源码生成候选 ZIP](../../docs/ANDROID_CANDIDATE_BUILDER.md)

## 组装 AAR

在 `bindings/android` 下运行。下面是 PowerShell 示例，把 `MeguvizMemory-Android-Candidate.zip` 换成自己持有的文件名，并把算出的摘要与候选提供方记录的值核对。合并用的中间文件为仓库根目录下的 `dist/MeguvizMemory-universal.aar`。

```powershell
$candidate = (Resolve-Path '../../dist/MeguvizMemory-Android-Candidate.zip').Path
$digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidate).Hash.ToLowerInvariant()
.\gradlew.bat :mgmem-android:assembleRelease "-Pmgmem.candidateZip=$candidate" "-Pmgmem.candidateSha256=$digest"
```

最终文件在 `mgmem-android/build/outputs/aar/mgmem-android-release.aar`。它含 Java 类、consumer R8 规则、三个 ABI 的 `libmgmem.so` 和 `libmgmem_pressure_jni.so`，以及供 CMake 使用的 Prefab 模块。Gradle 从候选提取共享库，组装时不会重新编译原生代码。

## 分发到本地仓库

沿用上面的 `$candidate` 与 `$digest`。

```powershell
.\gradlew.bat :mgmem-android:publishCentralPublicationToCentralStagingRepository "-Pmgmem.candidateZip=$candidate" "-Pmgmem.candidateSha256=$digest"
```

默认输出在 **MeguvizMemory 仓库根目录**的 `build/central-staging/`，可用 `-PcentralStagingDir=<路径>` 指定其他位置。工件坐标为 `io.github.jichuo1.meguviz:mgmem-android:1.2.1`，版本来自[gradle.properties](gradle.properties)。除 AAR 外，目录还包含 POM、Gradle 模块元数据、源码和 Javadoc 包。

应用可在 `settings.gradle.kts` 的现有仓库配置中加入下面的代码，并在运行 Gradle 时通过 `-PmgmemRepo=<本地仓库的绝对路径>` 提供位置。参数指向含 `io/` 的仓库根目录。

```kotlin
dependencyResolutionManagement {
    repositories {
        providers.gradleProperty("mgmemRepo").orNull?.let { repo ->
            maven {
                url = uri(repo)
                content { includeGroup("io.github.jichuo1.meguviz") }
            }
        }
    }
}
```

这条发布命令的目标是本地 Maven 仓库。需要签名时，使用已配置的 GPG 密钥并传 `-Pmeguviz.sign=true`。公开上传是独立的发布操作。
