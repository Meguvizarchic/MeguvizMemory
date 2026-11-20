import java.util.zip.ZipFile

plugins {
    alias(libs.plugins.android.library)
    `maven-publish`
    signing
}

val publicationVersion = providers.gradleProperty("meguviz.version").orElse("1.2.2")
val centralStagingDir = providers.gradleProperty("centralStagingDir").orElse("../../build/central-staging")
val signCentral = providers.gradleProperty("meguviz.sign").map {
    require(it == "true" || it == "false") { "meguviz.sign must be true or false" }
    it == "true"
}.orElse(false)

// The universal AAR is produced by the repository's frozen candidate tool
// (tools/merge_android_candidate_aar.py), never rebuilt inside Gradle.
val mergedAarFile = rootProject.file("../../dist/MeguvizMemory-universal.aar")
val jniDir = layout.buildDirectory.dir("jni")
val candidateZip = providers.gradleProperty("mgmem.candidateZip")
    .orElse(rootProject.file("dist/MeguvizMemory-Android-Candidate.zip").absolutePath)
val candidateSha = providers.gradleProperty("mgmem.candidateSha256").orElse("")

val mergeUniversalAar = tasks.register<Exec>("mergeUniversalAar") {
    description = "Merge the frozen candidate ZIP into one universal AAR."
    inputs.file(candidateZip.get())
    inputs.property("sha", candidateSha.get())
    outputs.file(mergedAarFile)
    doFirst {
        mergedAarFile.delete()
    }
    commandLine("python", rootProject.file("../../tools/merge_android_candidate_aar.py").absolutePath,
        "--candidate", candidateZip.get(),
        "--candidate-sha256", candidateSha.get(),
        "--output", mergedAarFile.absolutePath)
    doLast {
        check(mergedAarFile.isFile) { "mergeUniversalAar produced no AAR" }
    }
}

val extractJni = tasks.register<Sync>("extractJni") {
    description = "Extract jni/<abi>/*.so from the merged AAR for packaging."
    dependsOn(mergeUniversalAar)
    into(jniDir)
    from(zipTree(mergedAarFile)) {
        include("jni/**")
        eachFile {
            path = relativePath.pathString.removePrefix("jni/")
        }
        includeEmptyDirs = false
    }
}

android {
    sourceSets {
        getByName("main") {
            java.srcDirs("../../../adapters/android/java")
        }
    }
    namespace = "io.github.jichuo1.meguviz.memory.android"
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }
    buildToolsVersion = "37.0.0"
    ndkVersion = "30.0.16248370"
    defaultConfig {
        minSdk = 27
        consumerProguardFiles("consumer-rules.pro")
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
    sourceSets {
        getByName("main") {
            @Suppress("DEPRECATION")
            jniLibs.srcDirs(jniDir.get().asFile)
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    // Prefab metadata is generated below (generatePrefabTree) and merged
    // into the AAR: AGP's prefabPublishing cannot publish libraries that
    // only exist as prebuilt jniLibs without an externalNativeBuild.
    publishing {
        singleVariant("release") {
            withSourcesJar()
            withJavadocJar()
        }
    }
}

tasks.withType<Javadoc>().configureEach {
    isFailOnError = true
    options.encoding = "UTF-8"
    (options as StandardJavadocDocletOptions).addStringOption("Xdoclint:none", "-quiet")
}

tasks.named("preBuild") { dependsOn(extractJni) }

// Hand-authored Prefab package (schema 2) over the same candidate bytes,
// so CMake consumers can find_package(mgmem-android) and link
// mgmem-android::mgmem / ::mgmem_pressure_jni with the public headers.
val prefabDir = layout.buildDirectory.dir("prefabTree")
val generatePrefabTree = tasks.register("generatePrefabTree") {
    description = "Generate the prefab/ tree merged into the AAR."
    dependsOn(extractJni)
    val includeDir = rootProject.file("../../include")
    inputs.dir(jniDir)
    inputs.dir(includeDir)
    inputs.property("version", publicationVersion)
    outputs.dir(prefabDir)
    doLast {
        val root = prefabDir.get().asFile
        root.deleteRecursively()
        val prefabRoot = root.resolve("prefab")
        prefabRoot.mkdirs()
        // Prefab package metadata is schema 1 in the current CLI: the
        // dependencies field is mandatory even when empty.
        prefabRoot.resolve("prefab.json").writeText(
            "{\"name\":\"mgmem-android\",\"schema_version\":1,\"dependencies\":[]," +
                "\"version\":\"${publicationVersion.get()}\"}\n")
        val modules = linkedMapOf(
            "mgmem" to ("libmgmem.so" to ""),
            "mgmem_pressure_jni" to ("libmgmem_pressure_jni.so" to ":mgmem"),
        )
        val abis = jniDir.get().asFile.listFiles { f -> f.isDirectory }
            ?.map { it.name }?.sorted().orEmpty()
        require(abis.isNotEmpty()) { "extractJni produced no ABI directories" }
        for ((module, spec) in modules) {
            val (library, export) = spec
            val moduleDir = prefabRoot.resolve("modules/$module")
            includeDir.copyRecursively(moduleDir.resolve("include"), overwrite = true)
            // library_name omitted: it defaults to lib<module>.so, which
            // is exactly the candidate file name for both modules.
            moduleDir.resolve("module.json").writeText(
                "{\"export_libraries\":[" +
                    (if (export.isEmpty()) "" else "\"$export\"") + "],\"android\":{}}\n")
            for (abi in abis) {
                val source = jniDir.get().asFile.resolve("$abi/$library")
                require(source.isFile) { "missing $library for $abi" }
                val libDir = moduleDir.resolve("libs/android.$abi")
                libDir.mkdirs()
                source.copyTo(libDir.resolve(library), overwrite = true)
                libDir.resolve("abi.json").writeText(
                    "{\"abi\":\"$abi\",\"api\":27,\"ndk\":30,\"stl\":\"none\"}\n")
            }
        }
    }
}

// AGP registers the AAR bundling task lazily (its name varies by AGP
// version), so hook every Zip task whose name looks like the AAR bundle.
tasks.withType<org.gradle.api.tasks.bundling.Zip>().configureEach {
    if (name.contains("bundle", ignoreCase = true) && name.contains("aar", ignoreCase = true)) {
        dependsOn(generatePrefabTree)
        from(prefabDir)
    }
}

tasks.register("verifyNoExternalRuntime") {
    val runtimeClasspath = configurations.named("releaseRuntimeClasspath")
    doLast {
        check(runtimeClasspath.get().files.isEmpty()) {
            "Android runtime dependencies must be explicitly reviewed before use"
        }
    }
}
tasks.matching { it.name == "preReleaseBuild" }.configureEach {
    dependsOn("verifyNoExternalRuntime")
}

publishing {
    publications {
        register<MavenPublication>("central") {
            groupId = "io.github.jichuo1.meguviz"
            artifactId = "mgmem-android"
            version = publicationVersion.get()
            afterEvaluate { from(components["release"]) }
            pom {
                name = "MeguvizMemory Android"
                description = "Explicit CPU memory domains, bounded Arena and ownership/borrow leases with Android JNI adapters."
                url = "https://github.com/Meguvizarchic/MeguvizMemory"
                licenses { license { name = "The Apache License, Version 2.0"; url = "https://www.apache.org/licenses/LICENSE-2.0.txt" } }
                developers { developer { id = "jichuo1"; name = "jichuo1"; url = "https://github.com/jichuo1" } }
                scm {
                    url = "https://github.com/Meguvizarchic/MeguvizMemory"
                    connection = "scm:git:https://github.com/Meguvizarchic/MeguvizMemory.git"
                    developerConnection = "scm:git:ssh://git@github.com/Meguvizarchic/MeguvizMemory.git"
                }
            }
        }
    }
    repositories {
        maven { name = "CentralStaging"; url = uri(rootProject.file(centralStagingDir.get())) }
    }
}

tasks.matching { it.name == "publishCentralPublicationToCentralStagingRepository" }.configureEach {
    dependsOn("mergeUniversalAar")
}
if (signCentral.get()) {
    signing {
        useGpgCmd()
        sign(publishing.publications["central"])
    }
}
