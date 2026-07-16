import org.gradle.api.tasks.Copy

plugins {
    id("com.android.library")
}

// Maps the sanitized Gradle flavor name (e.g. "archarmv8_4_a") back to the
// real -march= value (e.g. "armv8.4-a") used for CMake and the final .so
// filename. Populated below while declaring the flavors; read back in
// androidComponents.onVariants(). Kept as a plain Kotlin map instead of
// Gradle's ExtraPropertiesExtension ("ext"), whose generic has<T>()/get<T>()
// overloads Kotlin couldn't resolve here.
val archByFlavor = mutableMapOf<String, String>()

android {
    namespace = "net.rpcs3.android"
    compileSdk = 36
    ndkVersion = (project.findProperty("rpcs3.ndkVersion") as String?) ?: "29.0.13113456"

    defaultConfig {
        minSdk = 29
        ndk {
            // All seven variants below are arm64 CPU-level tunings; x86_64
            // is emitted untuned (matches rpcsx-ui-android's fallback path).
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                // CI passes -Prpcs3.ccache=true (with ccache on PATH) so the
                // LLVM build is cached between workflow runs.
                if ((project.findProperty("rpcs3.ccache") as String?)?.toBoolean() == true) {
                    arguments += listOf(
                        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
                    )
                }
            }
        }
    }

    buildTypes {
        release {
            externalNativeBuild {
                cmake {
                    // AGP defaults release variants to RelWithDebInfo; with a
                    // statically linked LLVM the debug-info intermediates are
                    // enormous (don't fit on GitHub runners). Upstream rpcsx
                    // ships plain Release + llvm-strip, so match that.
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = (project.findProperty("rpcs3.cmakeVersion") as String?) ?: "3.31.6"
        }
    }

    // One product flavor per ARMv8/v9 micro-architecture level. Gradle
    // builds each as a normal variant (e.g. `assembleArmv84aRelease`);
    // `./gradlew assembleRelease` builds all of them in one shot.
    flavorDimensions += "armLevel"
    productFlavors {
        listOf(
            "armv8-a",
            "armv8.1-a",
            "armv8.2-a",
            "armv8.4-a",
            "armv8.5-a",
            "armv9-a",
            "armv9.1-a"
        ).forEach { arch ->
            // Gradle flavor names can't contain '.' or '-', so sanitize for
            // the identifier but keep the real -march= value for CMake and
            // for the final .so filename.
            val flavorName = "arch" + arch.replace(".", "_").replace("-", "_")
            archByFlavor[flavorName] = arch
            create(flavorName) {
                dimension = "armLevel"
                externalNativeBuild {
                    cmake {
                        arguments += "-DUSE_ARCH=$arch"
                    }
                }
                buildConfigField("String", "TARGET_ARCH", "\"$arch\"")
            }
        }
    }

    buildFeatures {
        buildConfig = true
    }
}

// ---------------------------------------------------------------------------
// After each variant's native build, copy+rename the produced
// librpcsx-android.so into the naming convention rpcsx-ui-android expects:
//   librpcsx-android-<abi>-<arch>.so
// Collected under I:\rpcs3\dist\ ready to attach to a GitHub release.
// ---------------------------------------------------------------------------
androidComponents {
    onVariants { variant ->
        val flavorArch = archByFlavor[variant.flavorName ?: ""] ?: return@onVariants

        val variantNameCap = variant.name.replaceFirstChar { it.uppercase() }

        val collectTask = tasks.register<Copy>("collectRpcsxSo${variantNameCap}") {
            group = "rpcsx"
            description = "Collects and renames the librpcsx-android.so built for $flavorArch"

            dependsOn("externalNativeBuild${variantNameCap}")

            from(layout.buildDirectory.dir("intermediates/cxx")) {
                include("**/obj/arm64-v8a/librpcsx-android.so")
                include("**/arm64-v8a/librpcsx-android.so")
            }
            includeEmptyDirs = false
            eachFile {
                path = "librpcsx-android-arm64-v8a-${flavorArch}.so"
            }
            into(rootProject.layout.projectDirectory.dir("dist"))
        }

        // `tasks.named(...)` requires the task to already be registered at
        // this point in configuration, but AGP creates `assemble<Variant>`
        // lazily and this androidComponents.onVariants callback can run
        // before that happens ("Task with name '...' not found"). `matching`
        // + `configureEach` instead reacts whenever that task shows up,
        // regardless of ordering.
        tasks.matching { it.name == "assemble${variantNameCap}" }.configureEach {
            finalizedBy(collectTask)
        }
    }
}
