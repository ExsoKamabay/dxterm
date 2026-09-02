#!/usr/bin/env bash
# Build and run the host-side engine tests. No Android SDK/NDK, no device, no network.
#
#   ./native-tests/run-tests.sh
#
# Exits non-zero if any test fails, so it drops straight into CI.
set -euo pipefail

cd "$(dirname "$0")"

if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build build --parallel >/dev/null
    exec ./build/terminal_engine_tests
fi

# Fallback for a machine without cmake: a direct compiler invocation.
echo "cmake not found; compiling directly with the system C++ compiler" >&2
CXX="${CXX:-g++}"
ENGINE=../app/src/main/cpp
mkdir -p build
"$CXX" -std=c++17 -Wall -Wextra -Wno-unused-parameter \
    -I "$ENGINE" -I "$ENGINE/engine" -I "$ENGINE/include" \
    -o build/terminal_engine_tests \
    terminal_engine_tests.cpp \
    "$ENGINE/engine/screen/ScreenBuffer.cpp" \
    "$ENGINE/engine/parser/AnsiParser.cpp" \
    "$ENGINE/engine/charset/Unicode.cpp" \
    "$ENGINE/engine/terminal/Terminal.cpp"
exec ./build/terminal_engine_tests
