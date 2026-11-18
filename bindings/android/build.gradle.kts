buildscript {
    dependencyLocking {
        lockMode = LockMode.STRICT
    }
    configurations.classpath {
        resolutionStrategy.activateDependencyLocking()
    }
}

plugins {
    alias(libs.plugins.android.library) apply false
    // Keep the host-aligned compiler on the classpath; AGP owns Kotlin application.
    alias(libs.plugins.kotlin.android) apply false
}
