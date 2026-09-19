cmake_minimum_required(VERSION 3.22.1)
project(xterm_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Native terminal engine (all logic lives on the C++ side per spec) ----
add_library(xterm SHARED
    native-lib.cpp
    engine/pty/Pty.cpp
    engine/parser/AnsiParser.cpp
    engine/screen/ScreenBuffer.cpp
    engine/charset/Unicode.cpp
    engine/terminal/Terminal.cpp
    engine/session/Session.cpp
)

# Root resolves "engine/..." and "core/..." includes (native-lib.cpp);
# engine/ resolves the engine's internal "screen/..." / "parser/..." /
# "charset/..." / "terminal/..." includes; include/ holds public xterm/Types.h.
target_include_directories(xterm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Android system libs
find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(xterm
    ${log-lib}
    ${android-lib}
)

# 16 KB page-size compatibility (Android 15+ / Google Play requirement).
# libxterm.so is built here by the local NDK, so it is forced to 16 KB PT_LOAD alignment below.
# NDK r26+ defaults to 16 KB, NDK r27+ makes it the only option; older NDKs default to 4 KB, hence
# the explicit flag. This only affects PT_LOAD segment alignment; it does not change runtime behaviour.
#
# The PREBUILT lib*.so in jniLibs/<abi> are NOT covered by this flag (they are shipped as-is and
# exec()'d from nativeLibraryDir): seven per ABI today -- libbusybox.so, libproot.so,
# libproot-loader.so, libtalloc.so, libandroid-shmem.so, libphdp.so and libvhdp-loader.so. All of
# them are 16 KB-aligned, each built with max-page-size=16384 by its own recipe under prebuilts/.
# The Gradle task validateNativeLibAlignment enforces that: it warns on every build and fails a
# release build while any prebuilt (any ABI) is < 16 KB-aligned.
target_link_options(xterm PRIVATE "-Wl,-z,max-page-size=16384")

# ---- VHDP rootfs-config bridge: libvhdpjni.so -> libvhdp.so (cpp/vhdp) ----
# The VHDP source lives here, under cpp/vhdp, and is built as its own CMake project with this
# app's NDK, ABI and STL. ANDROID_STL is c++_shared for the whole app, so libxterm, libvhdp and
# libvhdpjni load one libc++_shared.so; libvhdp's linker version script still keeps every C++
# symbol local. This is the APPLICATION half of VHDP: the app links libvhdp and uses it (via the
# JNI bridge below) to inspect and configure the rootfs, to decide the dpkg-recovery and system
# projection plans, and to render diagnostics (see BootManager, GuestBackend, Vhdp.kt).
#
# The CLI is deliberately NOT built here. It ships as fully STATIC prebuilts from this same
# source (prebuilts/vhdp-cli): jniLibs/<abi>/libphdp.so, which RUNS the terminal session as the
# rootless engine, with jniLibs/<abi>/libvhdp-loader.so as its userland ELF loader, plus
# assets/vhdp/bin/<abi>/vhdp, the in-guest `vhdp` command installed at launch by
# Bootstrap.installVhdpCommand -- kept apart so the app library and the executables never share
# a binary. Hence the CLI/tests/examples targets stay off here.
set(VHDP_BUILD_CLI OFF)
set(VHDP_BUILD_TESTS OFF)
set(VHDP_BUILD_EXAMPLES OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/vhdp
                 ${CMAKE_CURRENT_BINARY_DIR}/vhdp EXCLUDE_FROM_ALL)

# The bridge talks to libvhdp only through the public C ABI header.
add_library(vhdpjni SHARED vhdp/vhdp_jni.cpp)
set_target_properties(vhdpjni PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_options(vhdpjni PRIVATE -Wall -Wextra -Wshadow -Wconversion -Werror)
target_link_libraries(vhdpjni PRIVATE vhdp ${log-lib})
target_link_options(vhdpjni PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,defs")

# NOTE on prebuilt libs (libproot/libbusybox/libtalloc/libandroid-shmem/libproot-loader):
# these are shipped in jniLibs/arm64-v8a and are consumed at RUNTIME (exec/dlopen from
# nativeLibraryDir), not link-time. The engine spawns them via the PTY. They are packaged
# by Gradle jniLibs, so no add_library(IMPORTED) is required here.
