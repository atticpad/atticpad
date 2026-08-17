plugins {
    // No Kotlin plugin. AGP 9.0 ships built-in Kotlin support and REJECTS
    // org.jetbrains.kotlin.android outright, so the Kotlin compiler version
    // is AGP's to choose and scripts/android.env records which one that is
    // rather than pinning it.
    alias(libs.plugins.android.application)
}

// The product version has exactly one source of truth, and it is a C header:
// core/include/atticpad/version.h. It also feeds the Windows .exe VERSIONINFO
// and the 3DS CIA version, so a hand-copied duplicate here is a straggler
// waiting to ship the wrong number -- which is what it did until 0.4.0.
//
// Parsing fails loudly rather than defaulting: a build that cannot read the
// version should stop, not silently ship "unknown".
fun apadVersionName(): String {
    val header = rootProject.file("../../core/include/atticpad/version.h")
    require(header.isFile) { "cannot find ${header.path} -- the version header moved?" }
    val text = header.readText()
    fun field(name: String): String =
        Regex("""#define\s+APAD_VERSION_$name\s+(\d+)""").find(text)?.groupValues?.get(1)
            ?: error("APAD_VERSION_$name not found in ${header.path}")
    val suffix = Regex("""#define\s+APAD_VERSION_SUFFIX\s+"([^"]*)"""").find(text)?.groupValues?.get(1)
        ?: error("APAD_VERSION_SUFFIX not found in ${header.path}")
    return "${field("MAJOR")}.${field("MINOR")}.${field("PATCH")}$suffix"
}

android {
    namespace = "net.atticpad"
    compileSdk = 36
    buildToolsVersion = "37.0.0"
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "net.atticpad"
        // API 26: NotificationChannel and startForegroundService both arrived
        // here, and the foreground Service is this app's spine (docs/DESIGN.md §7.2)
        // rather than an optional extra.
        minSdk = 26
        targetSdk = 36
        // Bumped by hand, once per release tag. Android requires it to be
        // monotonic; it is deliberately NOT derived from the version string,
        // because a release candidate and its final release share a version
        // string suffix change but must still install over one another.
        versionCode = 7
        versionName = apadVersionName()

        ndk {
            // x86_64 is here for the emulator, not for a shipping device. It
            // is also the ABI where an endianness bug would NOT show up, so
            // the self-test result that matters is the one from an arm64
            // phone.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                // libapad is C99 with no C++ anywhere in it.
                arguments += "-DANDROID_STL=none"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"       // scripts/android.env APAD_CMAKE_VERSION
        }
    }

    // Release signing exists ONLY when the environment hands it a keystore,
    // which in practice means a tagged CI run holding the repository secrets.
    // A developer running `assembleRelease` still gets an unsigned apk, and a
    // fork with no secrets still builds -- both by construction rather than by
    // remembering not to commit a key. The keystore never exists in the repo.
    //
    // Losing this key is unrecoverable: Android will not replace an installed
    // app whose signature changed, so every user would have to uninstall
    // first. Back it up somewhere that survives this machine.
    val keystorePath: String? = System.getenv("APAD_ANDROID_KEYSTORE")
    if (keystorePath != null) {
        signingConfigs {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = System.getenv("APAD_ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("APAD_ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("APAD_ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        // A debug build installs ALONGSIDE a release one rather than
        // colliding with it. Without the suffix the two share an
        // applicationId, so installing either over the other fails on the
        // signature mismatch (debug key vs release key) and the only way
        // through is to uninstall first -- which takes the app's data with
        // it, and makes "test the fix against the shipped build" needlessly
        // destructive. Distinct ids mean both can sit on the phone at once.
        //
        // Only the APPLICATION ID is suffixed, never `namespace`: the JNI
        // entry points are named after the Java package (Java_net_atticpad_*
        // in apad_jni.c), and moving that would silently unbind every native
        // method at runtime.
        debug {
            applicationIdSuffix = ".debug"
            // So the version on screen and in a bug report says which build
            // it came from. The release build's name stays exactly
            // version.h's string.
            versionNameSuffix = "-debug"
            manifestPlaceholders["appLabel"] = "AtticPad debug"
        }
        release {
            isMinifyEnabled = false
            manifestPlaceholders["appLabel"] = "AtticPad"
            // Signed only when the environment supplied a keystore above;
            // otherwise deliberately unsigned. Releases come from tagged CI
            // only (docs/DESIGN.md §8.5).
            signingConfig = if (keystorePath != null) signingConfigs.getByName("release") else null
        }
    }

    // This app deliberately has ZERO third-party dependencies — no AndroidX,
    // no Compose, no Material. Everything it needs (Notification,
    // NsdManager, SensorManager, WifiManager, View) is in the framework at
    // API 26. The reason is not minimalism for its own sake: the phone is not
    // attached to this machine, so every dependency is a failure mode that
    // can only be diagnosed at arm's length.
    buildFeatures {
        buildConfig = false
        viewBinding = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            // Load libapadjni.so straight out of the apk rather than
            // extracting it at install time.
            useLegacyPackaging = false
        }
    }
}

// No kotlin { compilerOptions { jvmTarget } } block: under built-in Kotlin
// jvmTarget defaults to android.compileOptions.targetCompatibility, set to 17
// above. Stating it twice is two places to forget.
