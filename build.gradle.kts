// Top-level build file. This exists purely so that `gradlew` can drive the
// CMake/NDK build of the rpcs3 core into the librpcsx-android-*.so variants
// consumed by rpcsx-ui-android, without needing Visual Studio.
plugins {
    // Matches the AGP version rpcsx-ui-android already pins for this same
    // Gradle 9.5.1 wrapper (gradle/libs.versions.toml -> agp = "8.13.2").
    id("com.android.library") version "8.13.2" apply false
}
