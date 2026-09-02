# Host-side (NOT Android) test build for the terminal engine.
#
# Deliberately a separate project from app/src/main/cpp/CMakeLists.txt so that nothing here
# can affect the APK: Gradle never configures this directory. It compiles the production
# engine sources in place, with no copies and no shims, for the host toolchain.
#
# Pty.cpp, Session.cpp and native-lib.cpp are excluded: they need a real PTY, a thread and
# JNI. Everything that decides what gets painted lives in the four files below.
#
# Usage:  ./run-tests.sh      (or: cmake -B build && cmake --build build && ./build/terminal_engine_tests)

cmake_minimum_required(VERSION 3.16)
project(xterm_engine_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)

add_executable(terminal_engine_tests
    terminal_engine_tests.cpp
    ${ENGINE_DIR}/engine/screen/ScreenBuffer.cpp
    ${ENGINE_DIR}/engine/parser/AnsiParser.cpp
    ${ENGINE_DIR}/engine/charset/Unicode.cpp
    ${ENGINE_DIR}/engine/terminal/Terminal.cpp
)

target_include_directories(terminal_engine_tests PRIVATE
    ${ENGINE_DIR}
    ${ENGINE_DIR}/engine
    ${ENGINE_DIR}/include
)

target_compile_options(terminal_engine_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

enable_testing()
add_test(NAME terminal_engine_tests COMMAND terminal_engine_tests)
