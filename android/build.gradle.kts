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
// AGP keys each C++ configure under android/.cxx/<BuildType>/<hash>/<abi> and
// android/build/intermediates/cxx/<BuildType>/<hash>/, where <hash> is derived
// from the effective CMake arguments for that (buildType, flavor) pair. Every
// time those arguments drift between builds (e.g. a CMakeLists.txt/gradle
// change adds or removes a flag) AGP silently starts a brand new multi-GB
// build tree (static LLVM + the whole core) under a new hash and leaves the
// old one on disk forever - AGP never cleans these up itself.
//
// This prunes, per (BuildType, USE_ARCH) pair, every hash directory except
// the one that was touched most recently - i.e. the one the build that just
// finished actually used. Only removes directories that AGP itself manages
// (.cxx/ and build/intermediates/cxx/), never source.
// ---------------------------------------------------------------------------
val pruneStaleCxxCaches = tasks.register("pruneStaleCxxCaches") {
    group = "rpcsx"
    description = "Deletes orphaned android/.cxx build trees left behind when AGP's C++ configure hash changes for a (buildType, USE_ARCH) pair already built"

    doLast {
        val cxxSourceRoot = layout.projectDirectory.dir(".cxx").asFile
        val cxxIntermediatesRoot = layout.buildDirectory.dir("intermediates/cxx").get().asFile

        if (!cxxSourceRoot.isDirectory) {
            return@doLast
        }

        fun archOf(hashDir: File): String? {
            val configureCmd = File(hashDir, "arm64-v8a/configure_command.bat")
            if (!configureCmd.isFile) return null
            return Regex("-DUSE_ARCH=([^\"\\s]*)").find(configureCmd.readText())?.groupValues?.get(1)
        }

        fun lastActivity(buildType: String, hashDir: File): Long {
            val logsDir = File(cxxIntermediatesRoot, "$buildType/${hashDir.name}/logs/arm64-v8a")
            return logsDir.listFiles()?.maxOfOrNull { it.lastModified() } ?: hashDir.lastModified()
        }

        cxxSourceRoot.listFiles { f -> f.isDirectory }?.forEach { buildTypeDir ->
            val buildType = buildTypeDir.name
            val hashDirs = buildTypeDir.listFiles { f -> f.isDirectory } ?: return@forEach

            hashDirs
                .mapNotNull { hashDir -> archOf(hashDir)?.let { arch -> Triple(hashDir, arch, lastActivity(buildType, hashDir)) } }
                .groupBy { (_, arch, _) -> arch }
                .forEach { (arch, group) ->
                    if (group.size <= 1) return@forEach

                    val newest = group.maxByOrNull { (_, _, mtime) -> mtime }!!
                    group.filter { it !== newest }.forEach { (hashDir, _, _) ->
                        logger.lifecycle("Removing orphaned C++ build cache for USE_ARCH=$arch ($buildType/${hashDir.name}, superseded by ${newest.first.name})")
                        hashDir.deleteRecursively()
                        File(cxxIntermediatesRoot, "$buildType/${hashDir.name}").deleteRecursively()
                    }
                }
        }
    }
}

// ---------------------------------------------------------------------------
// After each variant's native build, copy+rename the produced
// librpcsx-android.so into the naming convention rpcsx-ui-android expects:
//   librpcsx-android-<abi>-<arch>.so
// Collected under <root>/dist/ ready to attach to a GitHub release.
// ---------------------------------------------------------------------------
androidComponents {
    onVariants { variant ->
        val flavorArch = archByFlavor[variant.flavorName ?: ""] ?: return@onVariants

        val variantNameCap = variant.name.replaceFirstChar { it.uppercase() }

        val collectTask = tasks.register<Copy>("collectRpcsxSo${variantNameCap}") {
            group = "rpcsx"
            description = "Collects and renames the librpcsx-android.so built for $flavorArch"

            if (variant.name.endsWith("Release", ignoreCase = true)) {
                dependsOn("strip${variantNameCap}DebugSymbols")
                from(layout.buildDirectory.dir("intermediates/stripped_native_libs/${variant.name}")) {
                    include("**/librpcsx-android.so")
                }
            } else {
                dependsOn("externalNativeBuild${variantNameCap}")
                from(layout.buildDirectory.dir("intermediates/cxx")) {
                    include("**/obj/arm64-v8a/librpcsx-android.so")
                    include("**/arm64-v8a/librpcsx-android.so")
                }
            }
            includeEmptyDirs = false
            eachFile {
                path = "librpcsx-android-arm64-v8a-${flavorArch}.so"
            }
            into(rootProject.layout.projectDirectory.dir("dist"))
            finalizedBy(pruneStaleCxxCaches)
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
