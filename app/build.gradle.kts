plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// ---------------------------------------------------------------------------------
// Release signing credentials, read from Gradle properties (or the matching
// ORG_GRADLE_PROJECT_* environment variables in CI). Nothing here reaches the
// repository: DRACOS_STORE_FILE points at a keystore kept outside the working tree.
// ---------------------------------------------------------------------------------
val signingStoreFile = project.findProperty("DRACOS_STORE_FILE") as String?
val signingStorePassword = project.findProperty("DRACOS_STORE_PASSWORD") as String?
val signingKeyAlias = project.findProperty("DRACOS_KEY_ALIAS") as String?
val signingKeyPassword = project.findProperty("DRACOS_KEY_PASSWORD") as String?

val signingMissing: List<String> = listOf(
    "DRACOS_STORE_FILE" to signingStoreFile,
    "DRACOS_STORE_PASSWORD" to signingStorePassword,
    "DRACOS_KEY_ALIAS" to signingKeyAlias,
    "DRACOS_KEY_PASSWORD" to signingKeyPassword
).filter { it.second.isNullOrBlank() }.map { it.first }

val signingConfigured: Boolean = signingMissing.isEmpty()

// The supported local-rootfs archive extensions, in one place: the Gradle validation
// below and RootfsArchive.classify() on the runtime side must agree on what counts as an
// image, or a file could be packaged that the app then refuses to read.
val rootfsArchiveRegex = Regex("\\.(tar\\.xz|txz|tar\\.gz|tgz|tar)$", RegexOption.IGNORE_CASE)
val bundledRootfsDir = file("src/main/assets/rootfs")

/** Archives (never README.txt, .gitkeep or any other non-archive file) in the local rootfs dir. */
fun bundledRootfsArchives(): List<File> =
    bundledRootfsDir.listFiles { f: File -> f.isFile && rootfsArchiveRegex.containsMatchIn(f.name) }
        .orEmpty().sortedBy { it.name }

// Which installer this build is is decided by ONE thing: whether an archive sits in
// src/main/assets/rootfs. Exactly one => the image ships inside the APK and first run
// extracts it offline. None => the APK carries no image and first run offers the
// catalogue in assets/rootfsURLS.json. There is no flavour, no build flag and nothing in
// the task name that says which; the packaged assets are the single source of truth, and
// the app reads the same directory back through AssetManager at runtime.
//
// More than one archive has no defined meaning: the app would have to guess which image
// the build intended, so the build refuses instead of shipping a coin flip.
val validateBundledRootfs = tasks.register("validateBundledRootfs") {
    group = "verification"
    description = "Fails when src/main/assets/rootfs holds more than one rootfs archive."
    val dir = bundledRootfsDir
    val regex = rootfsArchiveRegex
    outputs.upToDateWhen { false }
    doLast {
        val found = dir.listFiles { f: File -> f.isFile && regex.containsMatchIn(f.name) }
            .orEmpty().sortedBy { it.name }
        if (found.size > 1) {
            throw GradleException(
                "Only one offline rootfs is allowed in ${dir.path}, but ${found.size} archives are there:\n" +
                    found.joinToString("\n") { "  - " + it.name } +
                    "\n\nThe build cannot know which one the app should install. Keep exactly one and " +
                    "remove the rest, or empty the directory to build the online installer instead.\n" +
                    "Non-archive files (README.txt, .gitkeep, ...) are ignored and can stay."
            )
        }
        if (found.isEmpty()) {
            logger.lifecycle("[rootfs] no local image in ${dir.path} -> ONLINE installer (assets/rootfsURLS.json)")
        } else {
            logger.lifecycle("[rootfs] bundling ${found[0].name} -> OFFLINE installer")
        }
    }
}

// Every build path runs it: preBuild is upstream of assemble, bundle, lint and test alike.
tasks.matching { it.name == "preBuild" }.configureEach { dependsOn(validateBundledRootfs) }

// Refuses to produce an unsigned release.
//
// This lives in the task graph rather than in a configuration block because a configuration
// block runs on every Gradle invocation, which would stop a contributor with no signing
// material from running assembleDebug or lint at all.
gradle.taskGraph.whenReady {
    val buildsRelease = allTasks.any { t ->
        (t.name.startsWith("assemble") || t.name.startsWith("bundle") ||
            t.name.startsWith("package") || t.name.startsWith("install")) &&
            t.name.contains("Release")
    }
    if (buildsRelease) {
        if (!signingConfigured) {
            throw GradleException(
                "Release signing is not configured; missing " + signingMissing.joinToString(", ") +
                    ". Set them in ~/.gradle/gradle.properties. " +
                    "Refusing to build an unsigned release."
            )
        }
        val ks = file(signingStoreFile!!)
        if (!ks.isFile) {
            throw GradleException("Keystore not found at " + ks.absolutePath + " (DRACOS_STORE_FILE).")
        }
    }
}

android {
    namespace = "com.xdrac"
    compileSdk = 36

    // Pinned so a CI runner and a developer machine produce the same native code.
    // AGP would otherwise pick whatever NDK happens to be installed, which makes the
    // C++ engine's output depend on the machine. Any bump here is a deliberate change.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.xdrac"
        minSdk = 24            // forkpty/openpty + WindowInsets IME animation path supported; runtime-guarded below
        targetSdk = 36
        versionCode = 2
        versionName = "1.0.1"

        // Every shipped prebuilt (proot, busybox, talloc, shmem, loader) is arm64-v8a only.
        // Another ABI here builds an APK whose binaries cannot run on it.
        ndk { abiFilters += "arm64-v8a" }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    // One application, one build. The installer kind is not a flavour: it is decided by
    // whether src/main/assets/rootfs holds an archive (see validateBundledRootfs above), so
    // assembleDebug / assembleRelease / bundleRelease are the only variants there are.
    //
    // A local image is deliberately not committed: 200 MB in git history is paid for by
    // every clone, and through Git LFS it exhausts the free bandwidth quota after a handful
    // of fetches. ./scripts/fetch-rootfs.sh puts one in place when an offline build is wanted.

    // The prebuilt executables must exist as real files inside nativeLibraryDir so they
    // can be exec()'d on API 29+. useLegacyPackaging=true keeps them uncompressed/extracted.
    packaging {
        jniLibs {
            useLegacyPackaging = true
            // .so.2 style names are not extracted by the packager; we ship talloc as
            // libtalloc.so and recreate the libtalloc.so.2 symlink at runtime (Bootstrap).
        }
    }

    // Release signing.
    //
    // The keystore itself lives OUTSIDE the repository and is located through a Gradle
    // property, so no signing material and no path into a repo directory is committed.
    //
    // Recommended location: ~/.gradle/gradle.properties (chmod 600)
    //   DRACOS_STORE_FILE=/home/you/.android-keys/dracxterm-release.jks
    //   DRACOS_STORE_PASSWORD=...
    //   DRACOS_KEY_ALIAS=dracxterm
    //   DRACOS_KEY_PASSWORD=...
    // In CI, pass them as ORG_GRADLE_PROJECT_DRACOS_* environment variables.
    signingConfigs {
        create("release") {
            // Blank counts as "not set", the same way signingMissing above reads it. Without the
            // takeIf, a property present but empty reached file("") and threw at configuration
            // time, which killed assembleDebug and lint too -- the exact builds the guard below
            // is written to keep working for a contributor with no signing material.
            signingStoreFile?.takeIf { it.isNotBlank() }?.let { storeFile = file(it) }
            storePassword = signingStorePassword ?: ""
            keyAlias = signingKeyAlias ?: ""
            keyPassword = signingKeyPassword ?: ""

            // Signature schemes, chosen rather than inherited.
            //
            // v1 (JAR) is dead weight: minSdk is 24 and v2 works from 24, so a v1 signature
            // only enlarges the APK and slows verification.
            //
            // v3 matters more than it looks. It is the only scheme carrying a certificate
            // lineage, and that lineage is the one mechanism for rotating the release key
            // without orphaning every installed copy. The default for this module measured
            // as v2-only, which would have locked the app to its first key for ever. On
            // Android the signing key is the app's identity, so it is not left to a default.
            enableV1Signing = false
            enableV2Signing = true
            enableV3Signing = true
        }
    }

    buildTypes {
        debug {
            isJniDebuggable = true

        }
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )

            // The refusal to emit an unsigned release lives in the task graph above, so a
            // contributor with no signing material can still build and lint.
            if (signingConfigured) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { viewBinding = true }

    // aapt must not re-compress the rootfs archives: openFd().length stops being the real
    // size, and the images are already compressed.
    androidResources { noCompress += listOf("xz", "gz", "tgz", "txz", "tar", "ttf") }

    // The app carries its own language toggle, so both translations have to be present on the
    // device at all times. An App Bundle splits resources by language by default, which would
    // install only the locale the device happens to be in and leave the toggle switching to
    // strings that were never delivered.
    bundle {
        language { enableSplit = false }
    }

}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")

    // Stream-extract the .tar.xz and .tar.gz rootfs images.
    implementation("org.apache.commons:commons-compress:1.26.2")
    implementation("org.tukaani:xz:1.9")

    // Zstandard, for the official Ollama .tar.zst artifact. commons-compress and tukaani
    // cover xz and gzip only.
    //
    // Pinned to 1.5.7-12, and the digit matters. zstd-jni commit a05b6ad (2026-07-30)
    // raised the LIBRARY's own build from compileSdkVersion 26 to compileSdk 37. AGP
    // writes that into the AAR as minCompileSdk and checkDebugAarMetadata enforces it on
    // consumers, so 1.5.7-13 demands compileSdk >= 37 while :app compiles against 36, the
    // most AGP 8.11.1 supports:
    //
    //     git merge-base --is-ancestor a05b6ad <tag>
    //       v1.5.7-1 .. v1.5.7-12   does not contain the bump
    //       v1.5.7-13               contains it
    //
    // -12 is therefore the newest release compatible with compileSdk 36, and it is not a
    // functional downgrade: ZstdInputStream(InputStream)/read/close are identical in both,
    // both stream with the same ZSTD_DStreamInSize buffer, both are BSD 2-Clause, and -12
    // already links with -Wl,-z,max-page-size=16384 for Android 15's 16 KB pages. Raising
    // it needs compileSdk and AGP raised first.
    implementation("com.github.luben:zstd-jni:1.5.7-12@aar")

}
