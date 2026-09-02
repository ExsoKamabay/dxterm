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
# The five PREBUILT binaries in jniLibs/arm64-v8a are already 16 KB-aligned, but
# libxterm.so is built here by the local NDK. NDK r26+ defaults to 16 KB, NDK r27+
# makes it the only option; older NDKs default to 4 KB. Forcing max-page-size=16384
# guarantees a 16 KB-aligned libxterm.so regardless of the NDK version used to build.
# This only affects PT_LOAD segment alignment; it does not change runtime behaviour.
target_link_options(xterm PRIVATE "-Wl,-z,max-page-size=16384")

# NOTE on prebuilt libs (libproot/libbusybox/libtalloc/libandroid-shmem/libproot-loader):
# these are shipped in jniLibs/arm64-v8a and are consumed at RUNTIME (exec/dlopen from
# nativeLibraryDir), not link-time. The engine spawns them via the PTY. They are packaged
# by Gradle jniLibs, so no add_library(IMPORTED) is required here.
