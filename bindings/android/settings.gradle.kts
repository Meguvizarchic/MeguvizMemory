pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

fun requireGradleJdk17(version: JavaVersion) {
    check(version == JavaVersion.VERSION_17) {
        "MeguvizMemory Android builds require Gradle to run on JDK 17"
    }
}
requireGradleJdk17(JavaVersion.current())

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "MeguvizMemoryAndroid"
include(":mgmem-android")
