// clients/android/settings.gradle.kts
//
// This Gradle build is a LEAF of the atticpad repo, not the root of its own
// project: app/src/main/cpp/CMakeLists.txt reaches back up to core/ and shim/
// so there is exactly one copy of the codec. Building this directory outside
// a checkout fails loudly rather than silently vendoring a second protocol
// implementation.
pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "atticpad-android"
include(":app")
