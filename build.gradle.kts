// Root build file. Plugin versions declared here, applied per-module.
plugins {
    // AGP 8.11.x is the stable line that officially supports compileSdk/targetSdk = 36
    // (Android 16). AGP 8.5.2 was only tested up to API 34, which is inconsistent with the
    // compileSdk = 36 already set in app/build.gradle.kts. AGP 8.11.x requires Gradle 8.13+
    // (see gradle/wrapper/gradle-wrapper.properties) and JDK 17+.
    id("com.android.application") version "8.11.1" apply false
    // Kotlin intentionally left at 1.9.24: no proven incompatibility requires a bump.
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
